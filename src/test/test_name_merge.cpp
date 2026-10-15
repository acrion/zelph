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
#include "wikidata_dump.hpp"

#include "network/reasoning.hpp"

#include <filesystem>

using namespace zelph::test;
using zelph::network::adjacency_set;
using zelph::network::Node;

// ---------------------------------------------------------------------------
// Giving a node a name another node already holds in that language MERGES the
// two. That is deliberate, it is announced, and it is the way to say
// afterwards that a node one wrote by hand and an imported entity are the same
// thing -- a rule's transitivity predicate and the Wikidata property, say.
//
// A node IS the hash of what it is built from, though, so the moment one of
// them disappears every fact built on it carries an id computed from a
// component that is no longer there. Network::merge cannot repair that: it
// rewires edges, and what a triple IS does not belong in network.hpp. Two
// symptoms followed, both silent:
//
//   * the printed line stopped round-tripping. "a p b" whose subject merged
//     into c prints as "c p b", and re-entering that line created a SECOND
//     node -- after which every query and every derivation answered twice.
//   * a fact whose subject and predicate became the same node stopped being
//     readable at all: the subject == predicate reading verifies the hash by
//     construction, so a stale one is rejected and the fact answers nothing,
//     although .node still shows its neighbours.
//
// The repair reads what every dependent node is built from BEFORE the merge --
// afterwards the components no longer hash back to it -- and re-creates those
// nodes under the id their new components give them, folding each into an
// equal node that is already there.
//
// The cases below are the shapes that repair has to get right. Every one of
// them asks the same two questions: does the graph still ANSWER, and does the
// line it prints re-enter as the SAME node rather than as a duplicate.
// ---------------------------------------------------------------------------

namespace
{
    // ".name <node> <lang> <name>" twice on the same name is what triggers a
    // merge. Both nodes exist beforehand, which is the case the command
    // reports as "... are different nodes => Merging them".
    void merge_by_name(const zelph::console::Interactive& interactive,
                       const std::string&                 first,
                       const std::string&                 second)
    {
        interactive.process(".name " + first + " en shared");
        interactive.process(".name " + second + " en shared");
    }

    // The identifier `.node` prints for a statement: the way by which a user
    // finds the node associated with a statement to assign it a name.
    std::string node_id(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& statement)
    {
        collector.clear();
        interactive.process(".node " + statement);
        const std::string marker = "Node ID: ";
        for (const auto& event : collector.events())
            if (const auto pos = event.text.find(marker); pos != std::string::npos)
                return event.text.substr(pos + marker.size(), event.text.find_first_not_of("0123456789", pos + marker.size()) - pos - marker.size());
        return {};
    }

    // The command that gives the node `.node` finds for `statement` the
    // name "both"; the second such command asks for the merge.
    std::string name_both(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& statement)
    {
        const std::string id = node_id(collector, interactive, statement);
        REQUIRE_FALSE(id.empty());
        return ".name " + id + " both";
    }

    std::vector<std::string> sorted_answers(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& query)
    {
        collector.clear();
        interactive.process(query);
        std::vector<std::string> found = collect_answers(collector);
        std::ranges::sort(found);
        return found;
    }

    // The refusal of a merge by name, and what must remain true
    // afterwards: the graph keeps its node count, and `statement` keeps
    // its node.
    void check_merge_refused(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& command, const std::string& reason, const std::string& statement)
    {
        const std::size_t before = node_count(collector, interactive);
        const std::string id     = node_id(collector, interactive, statement);
        CHECK_THROWS_WITH_AS(interactive.process(command),
                             doctest::Contains("Merging the two nodes is impossible because " + reason),
                             std::runtime_error);
        CHECK(node_count(collector, interactive) == before);
        CHECK(node_id(collector, interactive, statement) == id);
    }
}

TEST_CASE("name merge: the surviving fact re-enters as the same node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p d
)");
        merge_by_name(interactive, "a", "c");

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p b"));
        CHECK(answers_contain(collector, "c p d"));
        CHECK(collect_answers(collector).size() == 2);

        // The line the engine just printed must denote the node it came
        // from. Before the repair this added a node and the query then
        // answered "c p b" twice.
        const std::size_t before = node_count(collector, interactive);
        interactive.process("c p b");
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).size() == 2); });
}

// The library's merge of a name pair, Zelph::set_name(name, name in another
// language, that language), behaves identically to how `.name` operates. It
// performed the merge without any plan and without any repair: `c p d`
// stayed under the identifier of its prior subject once `c` went into `a`,
// and upon re-entering the line it prints, it generated a second node.
TEST_CASE("name merge: the library's merge of a name pair rebuilds what is built on the vanished node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\nc p d\n.name c en cc\n");
        collector.clear();
        interactive.graph()->set_name("a", "cc", "en");
        CHECK(any_event_contains(collector, "Warning: (C) Merging Node"));

        CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b", "a p d"});
        const std::size_t before = node_count(collector, interactive);
        interactive.process("a p d");
        CHECK(node_count(collector, interactive) == before); });
}

// When two statements are combined under a shared name, they form a single
// node that holds the objects from both: following the merging of `a p b`
// and `a p c`, the survivor states `a p b c`. It kept the identifier of
// `a p c`, even though its current content no longer hashes to that
// identifier, causing the line it outputs not to re-enter as the identical
// node: `a p b c` added a new node, and `X p Y` returned each row twice.
// Where `a p b c` existed before the merge, the graph held that statement
// twice from the moment of merging onward. The merge is reached in the same
// way a user reaches it, using the identifiers that `.node` prints.
TEST_CASE("name merge: two statements merged into one re-enter as the survivor")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\na p c\n(a p b) bad j\n(a p c) bad k\n");
        SUBCASE("the merged statement is new") {}
        SUBCASE("the merged statement was there before") { interactive.process("a p b c"); }

        const std::string first  = node_id(collector, interactive, "a p b");
        const std::string second = node_id(collector, interactive, "a p c");
        REQUIRE_FALSE(first.empty());
        REQUIRE_FALSE(second.empty());
        interactive.process(".name " + first + " both");
        interactive.process(".name " + second + " both");

        collector.clear();
        interactive.process("X p Y");
        CHECK(collect_answers(collector).size() == 2);

        // The facts constructed upon either statement follow the survivor to
        // its new identifier, so they too re-enter as the same nodes.
        const std::size_t before = node_count(collector, interactive);
        process_lines(interactive, "a p b c\n(a p b c) bad j\n(a p b c) bad k\n");
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("X p Y");
        CHECK(collect_answers(collector).size() == 2);

        collector.clear();
        interactive.process("(a p b c) bad Z");
        std::vector<std::string> answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"both bad j", "both bad k"}); });
}

// The same holds when the statement containing more objects is merged into
// the one with fewer: `a p b c` holds the name, as it is named first, so
// the merge takes it into `a p b`, and the survivor holds the objects from
// both -- the content of the node that vanished, adopting its identifier.
// Before, it kept the id of `a p b`, and re-entering `a p b c` added a
// second node beside it.
TEST_CASE("name merge: a statement merged into one with fewer objects re-enters as the survivor")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b c\na p b\n(a p b) bad j\n(a p b c) bad k\n");
        const std::string wide   = node_id(collector, interactive, "a p b c");
        const std::string narrow = node_id(collector, interactive, "a p b");
        REQUIRE_FALSE(wide.empty());
        REQUIRE_FALSE(narrow.empty());
        interactive.process(".name " + wide + " both");
        interactive.process(".name " + narrow + " both");

        const std::size_t before = node_count(collector, interactive);
        process_lines(interactive, "a p b c\n(a p b c) bad j\n(a p b c) bad k\n");
        CHECK(node_count(collector, interactive) == before);
        CHECK(node_id(collector, interactive, "a p b c") == wide);

        collector.clear();
        interactive.process("X p Y");
        CHECK(collect_answers(collector).size() == 2);

        collector.clear();
        interactive.process("(a p b c) bad Z");
        std::vector<std::string> answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"both bad j", "both bad k"}); });
}

// The survivor of two statements may be a statement that is known to be
// wrong: `a p b` and `a p c` merge into `a p b c`, which `¬` refuted.
// This merge would make a claimed fact the same statement as a refuted
// one, and just as every such fold is refused before any change occurs,
// this one is also refused. As long as the survivor kept the identifier
// of `a p c`, the merge proceeded and resulted in a claimed `a p b c`
// existing alongside the refutation of that very same statement.
TEST_CASE("name merge: two statements whose survivor is refuted are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(a p b c)\na p b\na p c\n");
        const std::string first  = node_id(collector, interactive, "a p b");
        const std::string second = node_id(collector, interactive, "a p c");
        REQUIRE_FALSE(first.empty());
        REQUIRE_FALSE(second.empty());
        interactive.process(".name " + first + " both");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH_AS(interactive.process(".name " + second + " both"),
                             doctest::Contains("\"a p b c\" is known to be wrong"),
                             std::runtime_error);
        CHECK(node_count(collector, interactive) == before);
        CHECK(node_id(collector, interactive, "a p c") == second);

        collector.clear();
        interactive.process("X p Y");
        std::vector<std::string> answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"a p b", "a p c"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b c) ~ refuted"}); });
}

// The single merge of two terms that remains: two ground rules sharing a
// single condition constitute two statements regarding one subject (the
// condition) and one predicate (`=>`), and they merge into the rule
// encompassing the consequences of both. Green before the shapes were
// ruled, and pinned so that the rule keeps this case.
TEST_CASE("name merge: two ground rules of one condition merge into the rule with both consequences")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(a p b) => (x ok y)\n(a p b) => (z ok w)\n");
        interactive.process(name_both(collector, interactive, "(a p b) => (x ok y)"));
        interactive.process(name_both(collector, interactive, "(a p b) => (z ok w)"));

        CHECK(interactive.graph()->get_rules().size() == 1);
        process_lines(interactive, "a p b\n.run");
        CHECK(sorted_answers(collector, interactive, "S ok O") == std::vector<std::string>{"x ok y", "z ok w"}); });
}

// Two statements of distinct subjects do not merge: no statement of one
// subject states both, and a node bearing two subjects is not a zelph
// statement. Network::merge read the survivor as the statement of the node
// that remained, thus `a p b` merged with `c p b` answered `c p b` alone,
// and `(a p b) bad j` became `(c p b) bad j`. The message names the remedy,
// and the remedy works: merging the subjects folds the two statements into a
// single one.
TEST_CASE("name merge: two statements of different subjects are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\nc p b\n(a p b) bad j\n(c p b) bad k\n");
        interactive.process(name_both(collector, interactive, "a p b"));

        check_merge_refused(collector, interactive, name_both(collector, interactive, "c p b"),
                            "\"a p b\" and \"c p b\" differ in subject or predicate, and no single statement states both. To make them one statement, merge the subjects or predicates in which they differ.",
                            "c p b");
        CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b", "c p b"});
        CHECK(sorted_answers(collector, interactive, "S bad O") == std::vector<std::string>{"(c p b) bad k", "both bad j"});

        interactive.process(".name a c");
        CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"c p b"});

        // The limit is one that .help .name states.
        collector.clear();
        interactive.process(".help .name");
        CHECK(any_output_contains(collector, "statements that differ in subject or predicate")); });
}

// The same applies to two predicates: `a p b` merged with `a q b` answered
// `a q b` alone, while the claim `a p b` disappeared without a word.
TEST_CASE("name merge: two statements of different predicates are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\na q b\n");
        interactive.process(name_both(collector, interactive, "a p b"));

        check_merge_refused(collector, interactive, name_both(collector, interactive, "a q b"), "\"a p b\" and \"a q b\" differ in subject or predicate", "a q b");
        CHECK(sorted_answers(collector, interactive, "a R b") == std::vector<std::string>{"a p b", "a q b"}); });
}

// When both diverged, Network::merge generated the cross product: `a p b`
// and `c q d` answered `a q d` and `c p b` as well, resulting in 16
// statements for `a p b c` and `x q y z`. A rule's pattern, when combined
// with a claimed statement, takes the same refusal. Prior to this rule,
// those two SUBCASEs lived in the test case "a claimed statement merged by
// name with a rule's pattern stays claimed", which merged them into a
// single node stating both; refused now, the pattern stays the rule's
// pattern, the rule continues to be listed, and it fires once `a p b` is
// claimed. The library's merge of a name pair refuses in the identical
// manner.
TEST_CASE("name merge: two statements of different subjects and predicates are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("one object each")
        {
            process_lines(interactive, "a p b\nc q d\n");
            interactive.process(name_both(collector, interactive, "a p b"));
            check_merge_refused(collector, interactive, name_both(collector, interactive, "c q d"), "\"a p b\" and \"c q d\" differ in subject or predicate", "c q d");
            CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b"});
            CHECK(sorted_answers(collector, interactive, "S q O") == std::vector<std::string>{"c q d"});
        }
        SUBCASE("several objects each")
        {
            process_lines(interactive, "a p b c\nx q y z\n");
            interactive.process(name_both(collector, interactive, "a p b c"));
            check_merge_refused(collector, interactive, name_both(collector, interactive, "x q y z"), "\"a p b c\" and \"x q y z\" differ in subject or predicate", "x q y z");
            CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b", "a p c"});
            CHECK(sorted_answers(collector, interactive, "S q O") == std::vector<std::string>{"x q y", "x q z"});
        }
        SUBCASE("a rule's pattern named before a claimed statement")
        {
            process_lines(interactive, "(a p b) => (x ok y)\nc q d\n");
            interactive.process(name_both(collector, interactive, "a p b"));
            check_merge_refused(collector, interactive, name_both(collector, interactive, "c q d"), "\"a p b\" and \"c q d\" differ in subject or predicate", "c q d");
        }
        SUBCASE("a claimed statement named before a rule's pattern")
        {
            process_lines(interactive, "(a p b) => (x ok y)\nc q d\n");
            interactive.process(name_both(collector, interactive, "c q d"));
            check_merge_refused(collector, interactive, name_both(collector, interactive, "a p b"), "\"c q d\" and \"a p b\" differ in subject or predicate", "a p b");
        }
        SUBCASE("through the library")
        {
            process_lines(interactive, "(a p b) => (x ok y)\nc q d\n");
            interactive.process(name_both(collector, interactive, "a p b"));
            const std::string other = node_id(collector, interactive, "c q d");
            interactive.process(".name " + other + " en other");
            const std::size_t before = node_count(collector, interactive);
            CHECK_THROWS_WITH_AS(interactive.graph()->set_name("both", "other", "en"),
                                 doctest::Contains("\"a p b\" and \"c q d\" differ in subject or predicate"),
                                 std::runtime_error);
            CHECK(node_count(collector, interactive) == before);
            CHECK(std::to_string(interactive.graph()->get_node("other", "en")) == other);
        }

        // On the pattern side: the rule remains active and fires on the
        // claim exclusively.
        if (interactive.graph()->rule_count() == 1)
        {
            CHECK(sorted_answers(collector, interactive, "S q O") == std::vector<std::string>{"c q d"});
            interactive.process(".run");
            CHECK(sorted_answers(collector, interactive, "S ok O").empty());
            process_lines(interactive, "a p b\n.run");
            CHECK(sorted_answers(collector, interactive, "S ok O") == std::vector<std::string>{"x ok y"});
        } });
}

// A rule merged with a statement: the rule's node paired with the
// statement's node. After merging, `.list-rules` found no rule, the
// survivor responded with `c => d` and `c q (x ok y)`, and `a p b` ceased
// to derive `x ok y`.
TEST_CASE("name merge: a rule and a statement are refused, and the rule keeps firing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(a p b) => (x ok y)\nc q d\n");
        std::string first  = name_both(collector, interactive, "(a p b) => (x ok y)");
        std::string second = name_both(collector, interactive, "c q d");
        std::string reason = "\"(a p b) => (x ok y)\" and \"c q d\" differ in subject or predicate";
        SUBCASE("the rule named first") {}
        SUBCASE("the statement named first")
        {
            std::swap(first, second);
            reason = "\"c q d\" and \"(a p b) => (x ok y)\" differ in subject or predicate";
        }
        interactive.process(first);

        check_merge_refused(collector, interactive, second, reason, "c q d");
        CHECK(interactive.graph()->rule_count() == 1);
        CHECK(sorted_answers(collector, interactive, "S q O") == std::vector<std::string>{"c q d"});
        process_lines(interactive, "a p b\n.run");
        CHECK(sorted_answers(collector, interactive, "S ok O") == std::vector<std::string>{"x ok y"}); });
}

// The occurs check: x = f(...x...) admits no finite term. `a p b` merged
// with its own subject turned into a node that references itself, and
// `a p b` was erased; when merged with `a p (a p b)`, the outer statement
// vanished.
TEST_CASE("name merge: a node and a term it is part of are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string first;
        std::string second;
        std::string reason;
        std::string kept;
        SUBCASE("a statement's subject named first")
        {
            interactive.process("a p b");
            // The atom undergoes a renaming via `.name a both`, and the
            // resulting message conveys the new designation.
            first  = ".name a both";
            second = name_both(collector, interactive, "a p b");
            reason = "both is part of \"both p b\", and a statement cannot be one of its own parts.";
            kept   = "both p b";
        }
        SUBCASE("a statement named before its subject")
        {
            interactive.process("a p b");
            first  = name_both(collector, interactive, "a p b");
            second = ".name a both";
            reason = "a is part of \"a p b\", and a statement cannot be one of its own parts.";
            kept   = "a p b";
        }
        SUBCASE("a nested statement named first")
        {
            process_lines(interactive, "a p b\na p (a p b)\n");
            first  = name_both(collector, interactive, "a p b");
            second = name_both(collector, interactive, "a p (a p b)");
            reason = "\"a p b\" is part of \"a p (a p b)\", and a statement cannot be one of its own parts.";
            kept   = "a p (a p b)";
        }
        SUBCASE("the outer statement named first")
        {
            process_lines(interactive, "a p b\na p (a p b)\n");
            first  = name_both(collector, interactive, "a p (a p b)");
            second = name_both(collector, interactive, "a p b");
            reason = "\"a p b\" is part of \"a p (a p b)\", and a statement cannot be one of its own parts.";
            kept   = "a p b";
        }
        SUBCASE("a set constant's member named first")
        {
            interactive.process("x has {b c}");
            first  = ".name b both";
            second = name_both(collector, interactive, "{b c}");
            reason = "both is part of {both c}, and a set constant cannot be one of its own members.";
            kept   = "{both c}";
        }
        interactive.process(first);

        check_merge_refused(collector, interactive, second, reason, kept); });
}

// This is the statement-level form of the refusal "one node is a variable,
// the other not": the variable is tied to the quantifier of its rule. The
// expression `a p X` from the rule `(a p X) => (X ok y)` merged with `a p c`
// to form `a p {X c}`, and the rule could no longer derive `d ok y` from
// `a p d`. The rule's condition is accessed via its identifier, as `.node a`
// lists it.
TEST_CASE("name merge: a statement over a rule's variable is refused, and the rule keeps firing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(a p X) => (X ok y)\na p c\n");
        auto* const z = interactive.graph();
        REQUIRE(z->get_rules().size() == 1);
        adjacency_set consequences;
        const Node    condition = z->parse_fact(*z->get_rules().begin(), consequences);
        REQUIRE(condition != 0);

        std::string first  = ".name " + std::to_string(condition) + " both";
        std::string second = name_both(collector, interactive, "a p c");
        SUBCASE("the pattern named first") {}
        SUBCASE("the claimed statement named first") { std::swap(first, second); }
        interactive.process(first);

        check_merge_refused(collector, interactive, second, "\"a p X\" holds a variable of a rule, and a statement over a rule's variable merges with no other.", "a p c");
        process_lines(interactive, "a p d\n.run");
        CHECK(sorted_answers(collector, interactive, "S ok O") == std::vector<std::string>{"c ok y", "d ok y"}); });
}

// An atom and a statement constitute a definition, x := t: the statement
// keeps its identifier, while the atom's facts are rebuilt over it,
// regardless of which of the two holds the name. Named in the reverse
// sequence, the merge was directed into the atom, which no content hashes
// to: `a p b` ceased to answer any query, and re-entering it made a second
// node. The library's merge of a name pair takes the identical direction.
TEST_CASE("name merge: an atom merged with a statement becomes the statement")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\nfoo q r\nm foo n\n");
        const std::string statement = node_id(collector, interactive, "a p b");
        REQUIRE_FALSE(statement.empty());

        SUBCASE("the atom named first")
        {
            interactive.process(".name foo both");
            interactive.process(".name " + statement + " both");
        }
        SUBCASE("the statement named first")
        {
            interactive.process(".name " + statement + " both");
            interactive.process(".name foo both");
        }
        SUBCASE("through the library")
        {
            interactive.process(".name " + statement + " en st");
            interactive.graph()->set_name("foo", "st", "en");
        }

        CHECK(node_id(collector, interactive, "a p b") == statement);
        CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b"});
        CHECK(sorted_answers(collector, interactive, "S q r").size() == 1);
        CHECK(sorted_answers(collector, interactive, "m R n").size() == 1);

        const std::size_t before = node_count(collector, interactive);
        process_lines(interactive, "a p b\n(a p b) q r\nm (a p b) n\n");
        CHECK(node_count(collector, interactive) == before); });
}

// A statement nested within a claimed fact is itself claimed (concepts.md,
// "Nested Facts"), and the fact rebuilt over the statement is claimed just
// as its typed line would be. `foo` merged with the pattern `a p b` of
// `(a p b) => (x ok y)` answered `(a p b) q r`, yet the rule was never
// fired, whereas the typed line `(a p b) q r` fires it. If the line would
// claim nothing, the rebuilt fact claims nothing either: within an opaque
// argument, or holding a rule's variable.
TEST_CASE("name merge: an atom merged with a rule's pattern claims it as its typed line does")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (x ok y)");
        const std::string pattern = name_both(collector, interactive, "a p b");
        bool              fires   = true;
        SUBCASE("a claimed fact about the atom, the atom named first")
        {
            process_lines(interactive, "foo q r\n.name foo both");
            interactive.process(pattern);
        }
        SUBCASE("a claimed fact about the atom, the pattern named first")
        {
            interactive.process("foo q r");
            interactive.process(pattern);
            interactive.process(".name foo both");
        }
        SUBCASE("a fact an opaque predicate mentions")
        {
            process_lines(interactive, "believes ~ opaque\nsue believes (foo q r)\n.name foo both");
            interactive.process(pattern);
            fires = false;
        }
        SUBCASE("a rule's pattern that holds a variable")
        {
            process_lines(interactive, "(foo q X) => (X z w)\n.name foo both");
            interactive.process(pattern);
            fires = false;
        }

        interactive.process(".run");
        CHECK(sorted_answers(collector, interactive, "S ok O") == (fires ? std::vector<std::string>{"x ok y"} : std::vector<std::string>{}));
        collector.clear();
        interactive.process(".node both");
        CHECK(any_output_contains(collector, "Rule pattern (not asserted)") == !fires); });
}

// A statement and a set constant represent two distinct types of term,
// with no term belonging to both categories simultaneously. Merged, a
// single node served as both a set and a statement concurrently, and was
// displayed as a collection once the set had vanished into it.
TEST_CASE("name merge: a statement and a set constant are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x has {b c}\na p b\n");
        std::string first  = name_both(collector, interactive, "{b c}");
        std::string second = name_both(collector, interactive, "a p b");
        std::string has    = "x has both";
        SUBCASE("the set constant named first") {}
        SUBCASE("the statement named first")
        {
            std::swap(first, second);
            has = "x has {b c}";
        }
        interactive.process(first);

        check_merge_refused(collector, interactive, second, "\"a p b\" is a statement and {b c} a set constant.", "a p b");
        CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b"});
        CHECK(sorted_answers(collector, interactive, "x has O") == std::vector<std::string>{has}); });
}

// H-8 of "name merge: two set constants merged into one re-enter as the
// survivor", which pinned the union `{b c d}` of `{b c}` and `{b d}`. A set
// constant IS its members (concepts.md), so `{b c}` = `{b d}` says that
// c = d, not that the set becomes `{b c d}`, and the union extended both
// sets. Refused now, both sets keep their members and their statements.
TEST_CASE("name merge: two set constants are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x has {b c}\ny has {b d}\n");
        interactive.process(name_both(collector, interactive, "{b c}"));

        check_merge_refused(collector, interactive, name_both(collector, interactive, "{b d}"), "{b c} and {b d} are set constants, and a set constant IS its members.", "{b d}");
        CHECK(sorted_answers(collector, interactive, "S has O") == std::vector<std::string>{"x has both", "y has {b d}"});
        CHECK(sorted_answers(collector, interactive, "A in {b d}") == std::vector<std::string>{"b in {b d}", "d in {b d}"});

        // Re-entered, the union constitutes a third set, the one it has
        // consistently denoted.
        const std::size_t before = node_count(collector, interactive);
        interactive.process("z has {b c d}");
        CHECK(node_count(collector, interactive) > before + 2);

        // The limit is one that .help .name states.
        collector.clear();
        interactive.process(".help .name");
        CHECK(any_output_contains(collector, "constants (a set constant IS its members")); });
}

// An atom and a set constant constitute a definition too: the set keeps its
// identifier, while the atom's facts are rebuilt over it. Named in the
// other order, the set was incorporated into the atom, and the statements
// about it answered through a node that is not a set. A member of the atom
// would become a member of the set, which IS its members: `x in foo` made
// the set contain x, thereafter it no longer hashed back to its id. Such a
// merge is refused.
TEST_CASE("name merge: an atom merged with a set constant becomes the set, unless it holds what the set does not")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x has {b c}\nfoo q r\n");
        const std::string set = node_id(collector, interactive, "{b c}");
        REQUIRE_FALSE(set.empty());

        SUBCASE("the atom named first")
        {
            interactive.process(".name foo both");
            interactive.process(".name " + set + " both");
        }
        SUBCASE("the set constant named first")
        {
            interactive.process(".name " + set + " both");
            interactive.process(".name foo both");
        }
        SUBCASE("the atom holds a member of the set")
        {
            interactive.process("b in foo");
            interactive.process(".name foo both");
            interactive.process(".name " + set + " both");
        }
        SUBCASE("the atom holds what the set does not")
        {
            interactive.process("y in foo");
            interactive.process(".name foo both");
            check_merge_refused(collector, interactive, ".name " + set + " both", "both holds y, which the set constant {b c} does not, and a set constant IS its members.", "{b c}");
            CHECK(sorted_answers(collector, interactive, "S in both") == std::vector<std::string>{"y in both"});
            return;
        }

        CHECK(node_id(collector, interactive, "{b c}") == set);
        CHECK(sorted_answers(collector, interactive, "S q O") == std::vector<std::string>{"both q r"});
        CHECK(sorted_answers(collector, interactive, "x has O") == std::vector<std::string>{"x has both"});
        const std::size_t before = node_count(collector, interactive);
        process_lines(interactive, "{b c} q r\nx has {b c}\n");
        CHECK(node_count(collector, interactive) == before); });
}

// The core node remains permanently, and it is impossible for it to
// transform into a statement, thus the statement entered the core node
// and was permanently lost.
TEST_CASE("name merge: a core node and a statement are refused, and nothing changes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".lang en\na p b\n");
        interactive.process(name_both(collector, interactive, "a p b"));
        const Node bang = interactive.graph()->get_core_node("!");

        check_merge_refused(collector, interactive, ".name ! both", "node " + std::to_string(bang) + " is a core node, and a core node never becomes a statement.", "a p b");
        CHECK(sorted_answers(collector, interactive, "S p O") == std::vector<std::string>{"a p b"}); });
}

// The identical merge on the route within a single language. ".name bob
// john", with both names in the current language, was refused -- "Names
// are unique per language; remove the other node" -- even though the
// documentation and .help promise the merge and the detour via a second
// language performed it. Removing the other node is the opposite of saying
// that the two are a single entity.
TEST_CASE("name merge: a name held in the same language merges the two nodes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
bob likes ann
bob knows carl
(bob likes ann) since 2020
john likes ann
john owns car
)");
        auto* const z    = interactive.graph();
        const Node  bob  = z->get_node("bob");
        const Node  john = z->get_node("john");

        collector.clear();
        interactive.process(".name bob john");
        CHECK(any_event_contains(collector, "Merging Node " + std::to_string(john) + " into Node " + std::to_string(bob)));

        // The node identified as first survives under the requested name,
        // while its old name has been removed.
        CHECK(z->get_node("john") == bob);
        CHECK(z->get_node("bob") == 0);

        // The facts of both end on the survivor, with `bob likes ann` and
        // `john likes ann` now forming a single statement, contained within
        // one node.
        collector.clear();
        interactive.process("john P O");
        CHECK(answers_contain(collector, "john likes ann"));
        CHECK(answers_contain(collector, "john knows carl"));
        CHECK(answers_contain(collector, "john owns car"));
        CHECK(collect_answers(collector).size() == 3);

        collector.clear();
        interactive.process("S since O");
        CHECK(answers_contain(collector, "(john likes ann) since 2020"));

        const std::size_t before = node_count(collector, interactive);
        process_lines(interactive, R"(
john likes ann
john owns car
(john likes ann) since 2020
)");
        CHECK(node_count(collector, interactive) == before); });
}

// The merge gives the requested name to the surviving node, so the label of
// the disappearing node in that language does not constitute a conflict. The
// transfer of names reported one regardless, and contrary to the outcome:
// after ".name bob john", it reported "Keeping existing name 'bob'", just one
// step before 'bob' was overwritten. A conflict in ANOTHER language is
// genuine and continues to be reported.
TEST_CASE("name merge: the name the merge assigns is not reported as a conflict")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const z = interactive.graph();

        SUBCASE("in the same language")
        {
            process_lines(interactive, R"(
bob likes ann
john owns car
)");
            const Node bob = z->get_node("bob");

            collector.clear();
            interactive.process(".name bob john");
            CHECK_FALSE(any_event_contains(collector, "Keeping existing name"));
            CHECK(z->get_name(bob, "zelph", false) == "john");
        }
        SUBCASE("across languages")
        {
            process_lines(interactive, R"(
a p b
c p d
.name a en shared
.name c en cc
)");
            const Node c = z->get_node("c");

            collector.clear();
            interactive.process(".name c en shared");
            CHECK_FALSE(any_event_contains(collector, "Keeping existing name 'cc'"));
            CHECK(any_event_contains(collector, "Keeping existing name 'c'"));
            CHECK(z->get_name(c, "en", false) == "shared");
        } });
}

TEST_CASE("name merge: a predicate merged into one of its own subjects stays readable")
{
    // The shape that made a fact vanish: `p` merged into `a` turns `a p b`
    // into a fact whose subject and predicate are the same node, and that
    // reading is only accepted when the hash agrees -- which after a merge it
    // did not. `.node` showed the neighbours, no query answered.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x p y
)");
        merge_by_name(interactive, "p", "a");

        collector.clear();
        interactive.process("S a O");
        CHECK(answers_contain(collector, "a a b"));
        CHECK(answers_contain(collector, "x a y"));

        // The declaration travelled with it, and there is exactly ONE of it:
        // the old `p ~ ->` folded into the `a ~ ->` the merge implies.
        collector.clear();
        interactive.process("S ~ ->");
        CHECK(answers_contain(collector, "a ~ ->"));

        collector.clear();
        interactive.process(".node x a y");
        CHECK(any_output_contains(collector, "Representation: x a y"));

        const std::size_t before = node_count(collector, interactive);
        interactive.process("x a y");
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("name merge: an object merged away keeps its fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
s p b
s q d
)");
        merge_by_name(interactive, "b", "d");

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "s p d"));

        const std::size_t before = node_count(collector, interactive);
        interactive.process("s p d");
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("name merge: a fact that becomes an existing one folds into it")
{
    // Two statements that were different become the SAME statement. One node
    // is the only right answer -- the alternative is a graph holding one
    // claim twice, with two derivations and two provenances.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p b
)");
        merge_by_name(interactive, "a", "c");

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p b"));
        CHECK(collect_answers(collector).size() == 1); });
}

// A fold that cannot be a single statement. `john sleeps nil` is a rule's
// pattern that `¬` has refuted, `jim sleeps nil` is claimed, and merging would
// equate them into one statement, with the probability 0 of the refutation and
// the 1 of the claim on a single edge, whereas a claimed fact is never
// refuted. Network::merge detected this halfway through, once the two atoms
// and their names had been merged, and raised an exception: `.list-rules`
// subsequently found no rule, and `X ~ refuted` responded with
// `refuted ~ refuted`. The merge is refused prior to any alteration in either
// direction, and the graph answers as before, also after a save and a load.
TEST_CASE("name merge: a fold that would make a refuted fact one statement with an unrefuted one is refused, and nothing changes")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_name_merge_refuted_fold.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\njim sleeps nil\n¬(john sleeps nil)");
        const Node        john   = graph->get_node("john");
        const Node        jim    = graph->get_node("jim");
        const std::size_t before = node_count(collector, interactive);

        const std::string reason = "'. Merging the two nodes is impossible because \"john sleeps nil\" is known to be wrong, and the merge would make it the same statement as \"jim sleeps nil\", which is not.";
        SUBCASE("the refuted fact folded into the claimed one")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name jim john"),
                                 doctest::Contains("Requested name 'john' is already used by node " + std::to_string(john) + " in language 'zelph" + reason),
                                 std::runtime_error);
        }
        SUBCASE("the claimed fact folded into the refuted one")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name john jim"),
                                 doctest::Contains("Requested name 'jim' is already used by node " + std::to_string(jim) + " in language 'zelph" + reason),
                                 std::runtime_error);
        }
        CHECK(node_count(collector, interactive) == before);

        const auto unchanged = [&]
        {
            CHECK(graph->get_node("john") == john);
            CHECK(graph->get_node("jim") == jim);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(mary waits nil) => (john sleeps nil)"));
            collector.clear();
            interactive.process("X ~ refuted");
            CHECK(collect_answers(collector) == std::vector<std::string>{"(john sleeps nil) ~ refuted"});
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"jim sleeps nil"});
            CHECK_THROWS_WITH(interactive.process("john sleeps nil"), doctest::Contains("known to be wrong"));
        };
        unchanged();
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        unchanged(); });
    std::filesystem::remove(file);
}

// An additional two routes lead to a fold that would make a refuted fact one
// statement with an unrefuted one. The refuted statement need not be built
// directly on the merged atom: here it is built on a fact that the merge
// rebuilds, and the plan follows every level before any alteration.
// Similarly, a name in another language merges the two atoms just as a name
// in zelph does. On both routes, Network::merge threw halfway, after the two
// atoms had already been merged.
TEST_CASE("name merge: a fold that meets a refuted fact a level deeper or under a name in another language is refused, and nothing changes")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_name_merge_refuted_fold_routes.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        const auto unchanged = [&](const std::string& rule, const std::string& query, const std::string& answer, const std::string& refutation, const std::string& wrong)
        {
            const auto check = [&]
            {
                collector.clear();
                interactive.process(".list-rules");
                CHECK(any_output_contains(collector, rule));
                collector.clear();
                interactive.process(query);
                CHECK(collect_answers(collector) == std::vector<std::string>{answer});
                collector.clear();
                interactive.process("X ~ refuted");
                CHECK(collect_answers(collector) == std::vector<std::string>{refutation});
                CHECK_THROWS_WITH(interactive.process(wrong), doctest::Contains("known to be wrong"));
            };
            check();
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            check();
        };

        SUBCASE("the refuted fact is built on the fact the merge rebuilds")
        {
            process_lines(interactive, "(mary waits nil) => ((john sleeps nil) worries ann)\n(jim sleeps nil) worries ann\n¬((john sleeps nil) worries ann)");
            const Node        john   = graph->get_node("john");
            const Node        jim    = graph->get_node("jim");
            const std::size_t before = node_count(collector, interactive);
            CHECK_THROWS_WITH_AS(interactive.process(".name jim john"),
                                 doctest::Contains("Requested name 'john' is already used by node " + std::to_string(john) + " in language 'zelph'. Merging the two nodes is impossible because \"(john sleeps nil) worries ann\" is known to be wrong, and the merge would make it the same statement as \"(jim sleeps nil) worries ann\", which is not."),
                                 std::runtime_error);
            CHECK(node_count(collector, interactive) == before);
            CHECK(graph->get_node("john") == john);
            CHECK(graph->get_node("jim") == jim);
            unchanged("(mary waits nil) => ((john sleeps nil) worries ann)", "S worries O", "(jim sleeps nil) worries ann", "((john sleeps nil) worries ann) ~ refuted", "(john sleeps nil) worries ann");
        }
        SUBCASE("the merge is asked for under a name in another language")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\njim sleeps nil\n¬(john sleeps nil)\n.name jim de x");
            const Node        john   = graph->get_node("john");
            const Node        jim    = graph->get_node("jim");
            const std::size_t before = node_count(collector, interactive);
            CHECK_THROWS_WITH_AS(interactive.process(".name john de x"),
                                 doctest::Contains("Requested name 'x' is already used by node " + std::to_string(jim) + " in language 'de'. Merging the two nodes is impossible because \"john sleeps nil\" is known to be wrong, and the merge would make it the same statement as \"jim sleeps nil\", which is not."),
                                 std::runtime_error);
            CHECK(node_count(collector, interactive) == before);
            CHECK(graph->get_node("john") == john);
            CHECK(graph->get_node("x", "de") == jim);
            CHECK(graph->get_name(john, "de", false).empty());
            unchanged("(mary waits nil) => (john sleeps nil)", "S sleeps O", "jim sleeps nil", "(john sleeps nil) ~ refuted", "john sleeps nil");
        } });
    std::filesystem::remove(file);
}

// Neither statement is there yet under the id the merge gives it:
// `jim likes john` and `john likes jim` each result in `jim likes jim`, with
// the merge creating it for the first and folding the second into it. Since
// both conflicting connections move and no connection that stays in place
// takes part, refuse_conflicting_merge must evaluate the relocated
// connections against one another. Network::merge threw during the second
// fold.
TEST_CASE("name merge: two facts the merge rebuilds into one new statement are refused when one of them is refuted")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        process_lines(interactive, "jim likes john\n¬(john likes jim)");
        const Node        john   = graph->get_node("john");
        const Node        jim    = graph->get_node("jim");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH_AS(interactive.process(".name jim john"),
                             doctest::Contains("Requested name 'john' is already used by node " + std::to_string(john) + " in language 'zelph'. Merging the two nodes is impossible because \"john likes jim\" is known to be wrong, and the merge would make it the same statement as \"jim likes john\", which is not."),
                             std::runtime_error);
        CHECK(node_count(collector, interactive) == before);
        CHECK(graph->get_node("john") == john);
        CHECK(graph->get_node("jim") == jim);
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"jim likes john"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(john likes jim) ~ refuted"});
        CHECK_THROWS_WITH(interactive.process("john likes jim"), doctest::Contains("known to be wrong")); });
}

// A weight assigned to an edge that is not a fact's edge to its predicate:
// zelph/set-weight assigns a weight to any edge, and the edge originating
// from `ann` to `jim likes ann` carries 0.2, whereas the one leading to
// `john likes ann` carries 1. The fold joins these two, an operation that
// Network::merge is not capable of performing. There is no refuted statement
// to name, thus the refusal names the two connections along with their
// respective weights.
TEST_CASE("name merge: a fold that would join two connections of conflicting weight is refused, and names both")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        process_lines(interactive, "jim likes ann\njohn likes ann\n%(zelph/set-weight \"ann\" (zelph/fact \"jim\" \"likes\" \"ann\") 0.2)");
        const Node        john   = graph->get_node("john");
        const Node        jim    = graph->get_node("jim");
        const std::string ann    = std::to_string(graph->get_node("ann"));
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH_AS(interactive.process(".name jim john"),
                             doctest::Contains("Requested name 'john' is already used by node " + std::to_string(john) + " in language 'zelph'. Merging the two nodes is impossible because it would join the connection from node " + ann + " to node "),
                             std::runtime_error);
        CHECK_THROWS_WITH(interactive.process(".name jim john"), doctest::Contains(", of probability 0.2, with the one from node " + ann + " to node "));
        CHECK_THROWS_WITH(interactive.process(".name jim john"), doctest::Contains(", of probability 1."));
        CHECK(node_count(collector, interactive) == before);
        CHECK(graph->get_node("john") == john);
        CHECK(graph->get_node("jim") == jim);
        collector.clear();
        interactive.process("S likes O");
        auto answers = collect_answers(collector);
        std::sort(answers.begin(), answers.end());
        CHECK(answers == std::vector<std::string>{"jim likes ann", "john likes ann"});
        CHECK(janet_value(collector, interactive, "(zelph/weight \"ann\" (zelph/fact \"jim\" \"likes\" \"ann\"))") == "0.2"); });
}

// A rule's pattern that nobody claimed and a refuted fact may be one
// statement: a second rule typed with `john sleeps nil` following
// `¬(john sleeps nil)` reuses the refuted node. A merge attempting to make
// the pattern `jim sleeps nil` of a second rule into the same statement was
// refused, as though the pattern had been claimed. It now folds in both
// directions, and the survivor is refuted: the graph answers as the typed
// rule leaves it, even after a save and reload. A claimed fact meeting a
// refuted one continues to be refused (as shown above).
TEST_CASE("name merge: a rule's pattern nobody claimed folds into a refuted fact and is refuted with it")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_name_merge_pattern_into_refuted.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(john sleeps nil)\n(ann waits nil) => (jim sleeps nil)");
        std::string statement;
        SUBCASE("the refuted fact folded into the pattern")
        {
            CHECK_NOTHROW(interactive.process(".name jim john"));
            statement = "john sleeps nil";
        }
        SUBCASE("the pattern folded into the refuted fact")
        {
            CHECK_NOTHROW(interactive.process(".name john jim"));
            statement = "jim sleeps nil";
        }

        const auto as_typed = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(mary waits nil) => (" + statement + ")"));
            CHECK(any_output_contains(collector, "(ann waits nil) => (" + statement + ")"));
            collector.clear();
            interactive.process("X ~ refuted");
            CHECK(collect_answers(collector) == std::vector<std::string>{"(" + statement + ") ~ refuted"});
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector).empty());
            CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("known to be wrong"));
        };
        as_typed();
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        as_typed();

        // The condition of either rule leads to a contradiction, just as does
        // the typed rule.
        collector.clear();
        process_lines(interactive, "ann waits nil\n.run");
        CHECK(has_contradiction(collector));
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".help .name");
        CHECK(any_output_contains(collector, "pattern nobody claimed folds into it")); });
    std::filesystem::remove(file);
}

// The same fold away from the merged atom. One level further down, the
// refuted statement and the pattern each hold an inner statement built on
// the merged atom. The merge of two predicates moves the connection to the
// predicate itself, which then ends at the merged node. And
// `(jim sleeps nil) worries john` and `(john sleeps nil) worries jim` both
// transform into a statement that the graph does not yet contain, so the
// refuted connection meets the pattern's while both move. Every such fold
// takes the refutation, just as the folds at the merged atom do.
TEST_CASE("name merge: a rule's pattern nobody claimed folds into a refuted fact a level deeper, through a merged predicate or as a new statement")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string statement;
        std::string query;
        SUBCASE("a level deeper, the refuted fact folded into the pattern")
        {
            process_lines(interactive, "(mary waits nil) => ((john sleeps nil) worries ann)\n¬((john sleeps nil) worries ann)\n(ann waits nil) => ((jim sleeps nil) worries ann)");
            CHECK_NOTHROW(interactive.process(".name jim john"));
            statement = "(john sleeps nil) worries ann";
            query     = "S worries O";
        }
        SUBCASE("a level deeper, the pattern folded into the refuted fact")
        {
            process_lines(interactive, "(mary waits nil) => ((john sleeps nil) worries ann)\n¬((john sleeps nil) worries ann)\n(ann waits nil) => ((jim sleeps nil) worries ann)");
            CHECK_NOTHROW(interactive.process(".name john jim"));
            statement = "(jim sleeps nil) worries ann";
            query     = "S worries O";
        }
        SUBCASE("the predicate of the pattern merged into that of the refuted fact")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(john sleeps nil)\n(ann waits nil) => (john rests nil)");
            CHECK_NOTHROW(interactive.process(".name sleeps rests"));
            statement = "john rests nil";
            query     = "S rests O";
        }
        SUBCASE("the predicate of the refuted fact merged into that of the pattern")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(john sleeps nil)\n(ann waits nil) => (john rests nil)");
            CHECK_NOTHROW(interactive.process(".name rests sleeps"));
            statement = "john sleeps nil";
            query     = "S sleeps O";
        }
        SUBCASE("both rebuilt into a new statement")
        {
            process_lines(interactive, "(ann waits nil) => ((jim sleeps nil) worries john)\n(mary waits nil) => ((john sleeps nil) worries jim)\n¬((john sleeps nil) worries jim)");
            CHECK_NOTHROW(interactive.process(".name jim john"));
            statement = "(john sleeps nil) worries john";
            query     = "S worries O";
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (" + statement + ")"));
        CHECK(any_output_contains(collector, "(ann waits nil) => (" + statement + ")"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(" + statement + ") ~ refuted"});
        CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("known to be wrong"));
        collector.clear();
        process_lines(interactive, "ann waits nil\n.run");
        CHECK(has_contradiction(collector));
        collector.clear();
        interactive.process(query);
        CHECK(collect_answers(collector).empty()); });
}

// The boundary of "nobody claimed" is the one that typing draws. A
// statement mentioned by an opaque predicate is not claimed, and
// `sue knows (john sleeps nil)` typed following the refutation shares the
// refuted node, thus the merge that makes the mention into the refuted
// statement folds. A statement that a fact of a regular predicate holds is
// claimed alongside it: typing `mary believes (john sleeps nil)` is refused
// as being known to be wrong, and so is the merge, which changes nothing.
TEST_CASE("name merge: a statement an opaque predicate mentions folds into a refuted fact, one that an ordinary fact holds is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(john sleeps nil)");
        SUBCASE("mentioned by an opaque predicate")
        {
            process_lines(interactive, "knows ~ opaque\nsue knows (jim sleeps nil)");
            CHECK_NOTHROW(interactive.process(".name jim john"));
            collector.clear();
            interactive.process("S knows O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"sue knows (john sleeps nil)"});
        }
        SUBCASE("held by a fact of an ordinary predicate")
        {
            CHECK_THROWS_WITH(interactive.process("mary believes (john sleeps nil)"), doctest::Contains("known to be wrong"));
            interactive.process("mary believes (jim sleeps nil)");
            CHECK_THROWS_WITH(interactive.process(".name jim john"), doctest::Contains("\"john sleeps nil\" is known to be wrong, and the merge would make it the same statement as \"jim sleeps nil\", which is not."));
            collector.clear();
            interactive.process("S believes O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"mary believes (jim sleeps nil)"});
        }
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(john sleeps nil) ~ refuted"});
        CHECK_THROWS_WITH(interactive.process("john sleeps nil"), doctest::Contains("known to be wrong")); });
}

// refuse_conflicting_merge can accept a single fold while refusing a later
// one. In `.name john jim`, the first pass meets the pattern
// `jim sleeps nil` moving onto the refuted `john sleeps nil` and accepts it;
// the second pass meets the refuted `jim runs nil` moving onto the claimed
// `john runs nil` and refuses it. The probability 0 that the accepted fold
// gives the pattern is written only once every pair has passed, thus the
// refused merge leaves the pattern unchanged, and its rule derives it.
TEST_CASE("name merge: a merge refused for a claimed fact leaves unrefuted the pattern it would have refuted")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(john sleeps nil)\n(ann waits nil) => (jim sleeps nil)\njohn runs nil\n(bob waits nil) => (jim runs nil)\n¬(jim runs nil)");
        const std::size_t before = node_count(collector, interactive);
        CHECK_THROWS_WITH(interactive.process(".name john jim"), doctest::Contains("\"jim runs nil\" is known to be wrong, and the merge would make it the same statement as \"john runs nil\", which is not."));
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("X ~ refuted");
        auto refutations = collect_answers(collector);
        std::sort(refutations.begin(), refutations.end());
        CHECK(refutations == std::vector<std::string>{"(jim runs nil) ~ refuted", "(john sleeps nil) ~ refuted"});
        collector.clear();
        interactive.process("ann waits nil");
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"jim sleeps nil"}); });
}

// The fold that is a single statement: a fact that was claimed and a rule's
// pattern. The survivor remains claimed, since a claim is never relinquished
// through a merge. The marking associated with the pattern was carried
// forward -- re-established on the survivor, or retained upon it when the
// claimed fact was folded into the pattern -- and `S sleeps O` no longer
// answered with the claim, which `.explain` identified as a rule pattern not
// asserted.
TEST_CASE("name merge: a claimed fact folded together with a rule's pattern stays claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the pattern folded into the claimed fact")
        {
            process_lines(interactive, "(mary waits nil) => (jon sleeps nil)\njim sleeps nil\n.name jim jon");
        }
        SUBCASE("the claimed fact folded into the pattern")
        {
            process_lines(interactive, "(mary waits nil) => (jim sleeps nil)\njon sleeps nil\n.name jim jon");
        }

        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"jon sleeps nil"});
        collector.clear();
        interactive.process(".explain (jon sleeps nil)");
        CHECK(any_output_contains(collector, "jon sleeps nil  [asserted"));
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (jon sleeps nil)")); });
}

// The claim of the two statements that the name joins, not of a fold of what
// is built on them: a claimed statement and a rule's pattern merged by
// `.name`. The pattern's marking is built on the pattern, so the survivor kept
// it or the merge rebuilt it there, and the claim vanished in either
// direction: the claimed statement failed to answer any query, `.node` called
// the survivor a rule pattern that is not asserted, and the rule over the
// pattern never fired. A claimed statement that nothing is built on, merged
// into the pattern, gave the repair no step whatsoever, and the repair
// concluded before reading the marking. Two statements sharing one subject and
// one predicate merge into the statement that holds the objects of both.
// The system rejects combinations of two statements involving distinct
// subjects and predicates ("name merge: two statements of different
// subjects and predicates are refused, and nothing changes").
TEST_CASE("name merge: a claimed statement merged by name with a rule's pattern stays claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto merge = [&](const std::string& first, const std::string& second)
        {
            const std::string first_id  = node_id(collector, interactive, first);
            const std::string second_id = node_id(collector, interactive, second);
            REQUIRE_FALSE(first_id.empty());
            REQUIRE_FALSE(second_id.empty());
            interactive.process(".name " + first_id + " both");
            interactive.process(".name " + second_id + " both");
        };
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            std::vector<std::string> found = collect_answers(collector);
            std::ranges::sort(found);
            return found;
        };
        interactive.process("(a p b) => (x ok y)");
        SUBCASE("statements of one subject and predicate with the pattern named first")
        {
            interactive.process("a p c");
            merge("a p b", "a p c");
            CHECK(answers("X p Y") == std::vector<std::string>{"a p b", "a p c"});
        }
        SUBCASE("statements of one subject and predicate with the claimed one named first")
        {
            interactive.process("a p c");
            merge("a p c", "a p b");
            CHECK(answers("X p Y") == std::vector<std::string>{"a p b", "a p c"});
        }

        collector.clear();
        interactive.process(".node both");
        CHECK_FALSE(any_output_contains(collector, "Rule pattern (not asserted)"));
        interactive.process(".run");
        CHECK(answers("S ok O") == std::vector<std::string>{"x ok y"}); });
}

// A statement that an opaque predicate mentions remains distinct from the
// claims, just as a rule's pattern does, and a claimed fact folded together
// with it continued to be treated as a mention: `S sleeps O` yielded no
// result, and `.explain` labelled it "mentioned; not asserted", regardless of
// the fold's direction. Entered in either sequence, the claim and the
// mention stand side by side, and so they do after the merge.
TEST_CASE("name merge: a claimed fact folded together with a statement an opaque predicate mentions stays claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the mention folded into the claimed fact")
        {
            process_lines(interactive, "believes ~ opaque\nsue believes (jon sleeps nil)\njim sleeps nil\n.name jim jon");
        }
        SUBCASE("the claimed fact folded into the mention")
        {
            process_lines(interactive, "believes ~ opaque\nsue believes (jim sleeps nil)\njon sleeps nil\n.name jim jon");
        }

        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"jon sleeps nil"});
        collector.clear();
        interactive.process(".explain (jon sleeps nil)");
        CHECK(any_output_contains(collector, "jon sleeps nil  [axiom]"));
        collector.clear();
        interactive.process("S believes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"sue believes (jon sleeps nil)"}); });
}

// A rule authored by zelph/mention bears the mark of a mention, and a fold
// connecting it to an asserted rule keeps the claim, just as it keeps the
// claim of any statement folded together with a mention: the survivor is
// the rule in force, regardless of the fold's direction. Two mentioned rules
// folded into a single one remain a mention, and no one has brought it into
// force.
TEST_CASE("name merge: folding a mentioned rule into an asserted one keeps it in force")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        constexpr const char* mention_jim = R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "mary" "waits" "nil") "=>" (zelph/fact "jim" "sleeps" "nil")))))";
        constexpr const char* mention_jon = R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "mary" "waits" "nil") "=>" (zelph/fact "jon" "sleeps" "nil")))))";
        bool                  in_force    = true;
        SUBCASE("the mention folded into the asserted rule")
        {
            interactive.process(mention_jim);
            interactive.process("(mary waits nil) => (jon sleeps nil)");
        }
        SUBCASE("the asserted rule folded into the mention")
        {
            interactive.process(mention_jon);
            interactive.process("(mary waits nil) => (jim sleeps nil)");
        }
        SUBCASE("two mentions")
        {
            interactive.process(mention_jim);
            interactive.process(mention_jon);
            in_force = false;
        }
        process_lines(interactive, ".name jim jon\nmary waits nil\n");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (jon sleeps nil)") == in_force);
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == (in_force ? std::vector<std::string>{"jon sleeps nil"} : std::vector<std::string>{})); });
}

// The same applies to the two statements that the name joins: a mention is
// marked as a rule's pattern is, and the merge rebuilt the marking on the
// survivor, so `X p Y` answered nothing and `.node` called the survivor
// mentioned and not asserted.
TEST_CASE("name merge: a claimed statement merged by name with a statement an opaque predicate mentions stays claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "believes ~ opaque\nsue believes (a p b)\na p c\n");
        std::string first  = node_id(collector, interactive, "a p b");
        std::string second = node_id(collector, interactive, "a p c");
        REQUIRE_FALSE(first.empty());
        REQUIRE_FALSE(second.empty());
        SUBCASE("the mention named first") {}
        SUBCASE("the claimed statement named first") { std::swap(first, second); }
        interactive.process(".name " + first + " both");
        interactive.process(".name " + second + " both");

        collector.clear();
        interactive.process("X p Y");
        std::vector<std::string> answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"a p b", "a p c"});
        collector.clear();
        interactive.process(".node both");
        CHECK_FALSE(any_output_contains(collector, "Mentioned (not asserted)"));
        collector.clear();
        interactive.process("S believes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"sue believes both"}); });
}

// The claim that a fold keeps is simultaneously the claim that the reasoning
// reads, where the pattern was a rule's condition: the rule fires on the next
// run, and a negated condition ceases to be valid. Previously, the fold left
// the claimed fact as a pattern, the rule did not fire, and `bob ~ awake` was
// derived under the assumption that `jon sleeps nil` was missing.
TEST_CASE("name merge: a claimed fact folded together with a rule's condition is the claim the next run reads")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the condition fires the rule")
        {
            process_lines(interactive, "(jon sleeps nil) => (mary waits nil)\njim sleeps nil\n.name jim jon\na b c");
            collector.clear();
            interactive.process("S waits O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"mary waits nil"});
        }
        SUBCASE("the negated condition no longer holds")
        {
            process_lines(interactive, "(X ~ person, ¬(jon sleeps nil)) => (X ~ awake)\njim sleeps nil\n.name jim jon\nbob ~ person");
            collector.clear();
            interactive.process("S ~ awake");
            CHECK(collect_answers(collector).empty());
        }

        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"jon sleeps nil"}); });
}

TEST_CASE("name merge: a nested fact is rebuilt at every level")
{
    // `(a p b) q c` is built on `a p b`, which is built on `a`. Repairing
    // only the inner level would leave the outer one stale, so the walk has
    // to reach upwards until nothing changes.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(a p b) q c
)");
        merge_by_name(interactive, "a", "z");

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "(z p b) q c"));

        const std::size_t before = node_count(collector, interactive);
        interactive.process("(z p b) q c");
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("name merge: a set constant is rebuilt before the facts that name it")
{
    // A set constant IS its members, so merging one of them changes the set's
    // own id -- and the membership facts are built FROM the set, not the
    // other way round. Rebuilding them in the order the walk finds them would
    // compute the fact's id from the set's OLD id and leave it stale a second
    // time, which is why the repair emits in dependency order.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x rel {a b}");
        merge_by_name(interactive, "a", "c");

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "c in {c b}"));
        CHECK(answers_contain(collector, "b in {c b}"));

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "x rel {c b}"));

        // The literal denotes the rebuilt set, which is what a set constant
        // promises: two occurrences are one node.
        const std::size_t before = node_count(collector, interactive);
        interactive.process("x rel {c b}");
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("name merge: a rule keeps firing on the node that survived")
{
    // Stefan's own use of the feature: a predicate written by hand in a rule,
    // merged afterwards with the entity an import brought in. The rule has to
    // go on working, and on the merged node.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X before Y, Y before Z) => (X before Z)
p1 before p2
p2 before p3
precedes coinedBy someone
)");
        merge_by_name(interactive, "before", "precedes");

        interactive.run(true, false, false);

        collector.clear();
        interactive.process("p1 precedes O");
        CHECK(answers_contain(collector, "p1 precedes p3"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "precedes"));
        CHECK_FALSE(any_output_contains(collector, "before"));

        // A fact derived through the merged rule explains without inventing a
        // second copy of its premise.
        collector.clear();
        interactive.process(".explain (p1 precedes p3)");
        CHECK(any_output_contains(collector, "p1 precedes p2"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]\n   └─ (p1 precedes p3)")); });
}

TEST_CASE("name merge: a collection of a rule's text is not merged with another node")
{
    // Merging a rule's own collection with a data node either pulls the data
    // node's facts into the rule's text -- the rule became `(X p Y) =>
    // (X likes zz)` and derived `a likes zz` -- or turns the rule's
    // collection into data. That is not two names for one thing,
    // hence the merge is refused in both directions, just as with a variable
    // and a constant, and both nodes keep their facts and their names.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
zz q yy
(X p Y) => (X likes @{d})
)");
        auto* const z = interactive.graph();

        Node rule       = 0;
        Node collection = 0;
        for (const Node r : z->get_rules())
        {
            adjacency_set consequences;
            z->parse_fact(r, consequences);
            adjacency_set objects;
            z->parse_fact(*consequences.begin(), objects);
            rule       = r;
            collection = *objects.begin();
        }
        REQUIRE(z->is_rule_template(collection));
        const Node data = z->get_node("zz");

        SUBCASE("the data node takes the name the collection holds")
        {
            interactive.process(".name " + std::to_string(collection) + " en tname");
            CHECK_THROWS_WITH_AS(interactive.process(".name zz en tname"),
                                 doctest::Contains("is a collection of a rule's text"),
                                 std::runtime_error);
            CHECK(z->get_name(collection, "en") == "tname");
        }
        SUBCASE("the collection takes the name the data node holds")
        {
            interactive.process(".name zz en zzz");
            interactive.process(".name " + std::to_string(collection) + " tname");
            CHECK_THROWS_WITH_AS(interactive.process(".name tname en zzz"),
                                 doctest::Contains("is a collection of a rule's text"),
                                 std::runtime_error);
            CHECK(z->get_name(data, "en") == "zzz");
            CHECK(z->get_name(collection) == "tname");
        }
        // The library's set_name(name, name in another language, that
        // language) merges the node referenced by the second name into the
        // holder associated with the first name, and this operation is
        // refused there before any modification of a name.
        SUBCASE("the library gives the data node the name the collection holds")
        {
            interactive.process(".name zz en zzz");
            interactive.process(".name " + std::to_string(collection) + " tname");
            CHECK_THROWS_WITH_AS(z->set_name("tname", "zzz", "en"),
                                 doctest::Contains("is a collection of a rule's text"),
                                 std::runtime_error);
            CHECK(z->get_name(data, "en") == "zzz");
            CHECK(z->get_name(collection) == "tname");
        }
        SUBCASE("the library gives the collection the name the data node holds")
        {
            interactive.process(".name " + std::to_string(collection) + " en tname");
            CHECK_THROWS_WITH_AS(z->set_name("zz", "tname", "en"),
                                 doctest::Contains("is a collection of a rule's text"),
                                 std::runtime_error);
            CHECK(z->get_name(collection, "en") == "tname");
        }

        CHECK(z->exists(collection));
        CHECK(z->exists(rule));
        CHECK(z->get_node("zz") == data);
        collector.clear();
        interactive.process("zz q O");
        CHECK(answers_contain(collector, "zz q yy"));

        // The rule continues to output its own collection, rather than
        // the data node.
        interactive.process("a p b");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).size() == 1);
        CHECK_FALSE(answers_contain(collector, "a likes zz")); });
}

// Within a single language, the merge is constrained in the same way as
// across languages, and each refusal names the one that is in effect.
// Previously, all of them read "Names are unique per language; remove the
// other node", which is not the reason in any of these cases. A rejected
// merge results in no change.
TEST_CASE("name merge: in the same language, what cannot merge is refused with its reason")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
john owns car
(X p Y) => (X q Y)
(X r Y) => (X likes @{d})
)");
        auto* const z = interactive.graph();

        Node collection = 0;
        for (const Node r : z->get_rules())
        {
            adjacency_set consequences;
            z->parse_fact(r, consequences);
            adjacency_set objects;
            z->parse_fact(*consequences.begin(), objects);
            if (z->is_rule_template(*objects.begin())) collection = *objects.begin();
        }
        REQUIRE(collection != 0);
        interactive.process(".name " + std::to_string(collection) + " tname");

        const std::size_t before = node_count(collector, interactive);

        SUBCASE("a core node's spelling")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name john nil"),
                                 doctest::Contains("A core node's name cannot be given away"),
                                 std::runtime_error);
        }
        SUBCASE("a variable and a non-variable")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name X john"),
                                 doctest::Contains("one node is a variable, the other not"),
                                 std::runtime_error);
        }
        SUBCASE("a collection of a rule's text")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name john tname"),
                                 doctest::Contains("is a collection of a rule's text"),
                                 std::runtime_error);
            CHECK(z->get_node("tname") == collection);
        }

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("john owns O");
        CHECK(answers_contain(collector, "john owns car")); });
}

// A rule's variable carries a display name exclusively: each statement
// quantifies its own, and a node seeking that name takes the lookup without
// merging ("names: .name takes a display name over from a variable"). The
// merge in one language reaches it from neither side. When two variables
// were involved, the request was refused with "Names are unique per
// language", which is inaccurate among variables; the refusal occurs because
// merging would make one rule's variable into another rule's.
TEST_CASE("name merge: a variable's display name is not a merge")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(A p B) => (A q B)
(C r D) => (C s D)
)");

        collector.clear();
        interactive.process(".name a A");
        CHECK(any_output_contains(collector, "display name of a variable"));
        CHECK_FALSE(any_event_contains(collector, "Merging"));

        CHECK_THROWS_WITH_AS(interactive.process(".name C D"),
                             doctest::Contains("Merging them would make the variable of one statement the variable of another"),
                             std::runtime_error);

        // Both rules keep their variables and continue to fire.
        process_lines(interactive, "x r y\n");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, R"("A" q b)"));
        collector.clear();
        interactive.process("S s O");
        CHECK(answers_contain(collector, "x s y")); });
}

// Across languages, `.name` merges two nodes that hold the respective names,
// and two variables are no exception: the merge persists, just as it does for
// any pair of nodes. Without a warning, its effect on variables is invisible.
// A variable belongs to the statement that quantifies it, so the second rule
// proceeds using the variable from the first rule and prints
// `(X r B) => (X s B)`. That same request within a single language is refused
// for this reason (refer to the earlier explanation); this instance merely
// states "Merging them", offering no alert, since the warning about a merge
// was omitted whenever the vanishing node was a variable. The library's
// cross-language overload took the same course without comment.
TEST_CASE("name merge: two variables merged across languages are announced as one variable two statements share")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X q Y)
.lang en
(A r B) => (A s B)
.lang zelph
)");

        SUBCASE("through .name")
        {
            collector.clear();
            interactive.process(".name X en A");
            CHECK(any_event_contains(collector, "Warning: (B) Merging variable node"));
            CHECK(any_event_contains(collector, "the statements that used these two now share one variable"));
            CHECK(any_output_contains(collector, "Merging them"));

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r B) => (X s B)"));
        }
        SUBCASE("through the library")
        {
            collector.clear();
            interactive.graph()->set_name("X", "A", "en");
            CHECK(any_event_contains(collector, "Warning: (C) Merging variable node"));
            CHECK(any_event_contains(collector, "the statements that used these two now share one variable"));
        } });
}

// The new name is a NAME. Previously, it was processed just like the node
// argument, meaning "14" referred to node 14 whenever no node bore the name
// "14" -- which blocked the rename with "already in use by node 14", and
// would now have merged a node the user never named.
TEST_CASE("name merge: a number given as the new name is a name, not a node id")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
bob likes ann
john owns car
)");
        auto* const       z    = interactive.graph();
        const Node        bob  = z->get_node("bob");
        const Node        john = z->get_node("john");
        const std::string id   = std::to_string(john);

        collector.clear();
        interactive.process(".name bob " + id);
        CHECK_FALSE(any_event_contains(collector, "Merging"));
        CHECK(z->get_node(id) == bob);
        CHECK(z->get_node("john") == john);

        collector.clear();
        interactive.process("john owns O");
        CHECK(answers_contain(collector, "john owns car")); });
}

TEST_CASE("name merge: the repaired graph survives a save/load round trip")
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "zelph_name_merge.bin";
    std::filesystem::remove(file);

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
(a p b) q c
x rel {a b}
)");
        merge_by_name(interactive, "a", "z");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    std::filesystem::remove(file);

    collector.clear();
    interactive.process("S q O");
    CHECK(answers_contain(collector, "(z p b) q c"));

    collector.clear();
    interactive.process("S rel O");
    CHECK(answers_contain(collector, "x rel {z b}"));

    // And the ids are the ones the statements denote, not the ones a merge
    // left behind: re-entering adds nothing.
    const std::size_t before = node_count(collector, interactive);
    interactive.process("(z p b) q c");
    CHECK(node_count(collector, interactive) == before);
}

// The same applies to merging within a single language, where the survivor
// is renamed instead of receiving a second name: the requested name is
// preserved, while the old one is not brought back upon loading.
TEST_CASE("name merge: a merge in the same language survives a save/load round trip")
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "zelph_name_merge_same_language.bin";
    std::filesystem::remove(file);

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
bob likes ann
(bob likes ann) since 2020
john likes ann
john owns car
)");
        interactive.process(".name bob john");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    std::filesystem::remove(file);

    CHECK(interactive.graph()->get_node("bob") == 0);

    collector.clear();
    interactive.process("john P O");
    CHECK(answers_contain(collector, "john likes ann"));
    CHECK(answers_contain(collector, "john owns car"));
    CHECK(collect_answers(collector).size() == 2);

    collector.clear();
    interactive.process("S since O");
    CHECK(answers_contain(collector, "(john likes ann) since 2020"));

    const std::size_t before = node_count(collector, interactive);
    interactive.process("(john likes ann) since 2020");
    CHECK(node_count(collector, interactive) == before);
}

TEST_CASE("name merge: merging INTO a core node keeps the core node")
{
    // The other direction, which the merge has always taken: a core node is
    // never the one that disappears. Its facts have to be repaired all the
    // same, since the node that merges into it is the one that vanishes.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
)");
        collector.clear();
        interactive.process(".name ! en contradiction");
        CHECK_FALSE(any_event_contains(collector, "Resource deadlock avoided"));

        collector.clear();
        interactive.process("! P O");
        CHECK(answers_contain(collector, "! is unsatisfiable"));

        // Re-entered in the language the statement was written in: 'is' and
        // 'unsatisfiable' are named in 'en' only, so the zelph spelling of
        // the same line would legitimately create two fresh nodes.
        interactive.process(".lang en");
        const std::size_t before = node_count(collector, interactive);
        interactive.process("contradiction is unsatisfiable");
        CHECK(node_count(collector, interactive) == before); });
}

// In the same direction within a single language: the core node is named
// first and takes the name, while the node that holds the name disappears
// into it.
TEST_CASE("name merge: merging INTO a core node in the same language keeps the core node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
)");
        auto* const z    = interactive.graph();
        const Node  bang = z->get_core_node("!");

        interactive.process(".name ! contradiction");
        CHECK(z->get_node("contradiction", "en") == bang);

        interactive.process(".lang zelph");
        collector.clear();
        interactive.process("! P O");
        CHECK(answers_contain(collector, "! is unsatisfiable")); });
}

// A core node's spelling ("~", "in", "nil", ...) denotes the core node in
// every language, thus it cannot serve as a name a user might assign to
// their own node. When applied across languages, it merged the node INTO the
// core node -- core nodes never disappear -- and the merged node's names
// then spelled the core node: following ".name prep3 en ~", every `~` fact
// printed as `prep3`, and "X ~ preposition" answered "prep3 prep3
// preposition". Within a single language, the same request was refused with
// "remove the other node", a directive no one can fulfil for a core node.
// `.name ! en contradiction` above represents the opposite direction, the
// core node taking a name, and remains valid.
TEST_CASE("name merge: a core node's spelling is not given away, in any language")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("prep3 ~ preposition");
        auto* const z     = interactive.graph();
        const Node  prep3 = z->get_node("prep3");
        const Node  tilde = z->get_core_node("~");

        const std::size_t before = node_count(collector, interactive);

        SUBCASE("across languages")
        {
            collector.clear();
            CHECK_THROWS_WITH_AS(interactive.process(".name prep3 en ~"),
                                 doctest::Contains("A core node's name cannot be given away"),
                                 std::runtime_error);
            // The merge is declared after it has taken place, meaning a
            // refused one is never declared at all.
            CHECK_FALSE(any_event_contains(collector, "Merging"));
        }
        SUBCASE("across languages, into a language nothing is named in yet")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name prep3 ace in"),
                                 doctest::Contains("A core node's name cannot be given away"),
                                 std::runtime_error);
        }
        SUBCASE("in the same language")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name prep3 nil"),
                                 doctest::Contains("A core node's name cannot be given away"),
                                 std::runtime_error);
        }
        // The two library calls that perform a merge based on a name
        // both reject it in the same manner, with the second doing so
        // before altering any name.
        SUBCASE("through the library")
        {
            CHECK_THROWS_WITH_AS(z->set_name(prep3, "~", "en", true),
                                 doctest::Contains("A core node's name cannot be given away"),
                                 std::runtime_error);
            interactive.process(".name prep3 en prepen");
            CHECK_THROWS_WITH_AS(z->set_name("~", "prepen", "en"),
                                 doctest::Contains("A core node's name cannot be given away"),
                                 std::runtime_error);
            CHECK(z->get_name(prep3, "en", false) == "prepen");
        }

        CHECK(node_count(collector, interactive) == before);
        CHECK(z->get_node("prep3") == prep3);
        CHECK(z->get_name(tilde, "zelph", false) == "~");
        CHECK(z->get_name(tilde, "en", false) == "~");

        collector.clear();
        interactive.process("X ~ preposition");
        CHECK(answers_contain(collector, "prep3 ~ preposition"));

        // The limit is one that .help .name states.
        collector.clear();
        interactive.process(".help .name");
        CHECK(any_output_contains(collector, "names it in every language and is never given")); });
}

// Two core nodes are both nodes the engine relies on, meaning neither can be
// absorbed into the other. Across languages they merged all the same once
// one of the nodes bore a name that the other had been assigned: following
// ".name ! en contradiction", ".name ~ en contradiction" merged `~` into
// `!`, causing every subsequent statement to fail with "requested right node
// 3 does not exist".
TEST_CASE("name merge: two core nodes are never merged")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
.name ! en contradiction
)");
        auto* const z     = interactive.graph();
        const Node  tilde = z->get_core_node("~");
        const Node  bang  = z->get_core_node("!");

        SUBCASE("across languages")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name ~ en contradiction"),
                                 doctest::Contains("both are core nodes"),
                                 std::runtime_error);
        }
        SUBCASE("in the same language")
        {
            interactive.process(".lang en");
            CHECK_THROWS_WITH_AS(interactive.process(".name ~ contradiction"),
                                 doctest::Contains("both are core nodes"),
                                 std::runtime_error);
            interactive.process(".lang zelph");
        }
        SUBCASE("through the library")
        {
            // `!` carries a name in the current language, `~` a
            // different one in another, and the call names `~` with
            // the name `!` holds.
            process_lines(interactive, R"(
.name ! bang
.name ~ de tilde
)");
            CHECK_THROWS_WITH_AS(z->set_name("bang", "tilde", "de"),
                                 doctest::Contains("both are core nodes"),
                                 std::runtime_error);
        }

        CHECK(z->exists(tilde));
        CHECK(z->exists(bang));
        CHECK_NOTHROW(interactive.process("a b c"));
        collector.clear();
        interactive.process("X ~ ->");
        CHECK(answers_contain(collector, "b ~ ->")); });
}

// The engine reads its own vocabulary by name within the language zelph: the
// node `refuted`, which a refutation's marking names, `opaque`, the
// rule-pattern predicate, and the other names of Zelph::engine_vocabulary. A
// rename that took such a name from its node, a `.delname`, or a name given to
// a different node changed what the engine reads, bypassing every refusal
// encountered by a statement concerning those nodes: `.name refuted gone`
// ended every refutation simultaneously. The name remains attached to its
// node just as a core node's spelling does, across every route, and the node
// does not merge with any other; each refusal leaves the refutation in force.
TEST_CASE("name merge: a name of the engine's vocabulary stays with its node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const z = interactive.graph();
        process_lines(interactive, "(x y z) => (a p b)\n¬(a p b)\nc q d");
        const Node refuted = z->get_node("refuted", "zelph");
        REQUIRE(refuted != 0);
        const std::string id    = std::to_string(refuted);
        const std::string taken = "Node " + id + " carries the name 'refuted' of the engine's vocabulary in language 'zelph'. A name of the engine's vocabulary cannot be taken from its node.";
        const std::string given = "Requested name 'refuted' belongs to the engine's vocabulary in language 'zelph', where it names node " + id + ". A name of the engine's vocabulary cannot be given away.";

        SUBCASE("taken by a rename")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name refuted gone"), doctest::Contains(taken), std::runtime_error);
        }
        SUBCASE("taken by a rename to a name another node holds")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name refuted c"), doctest::Contains(taken), std::runtime_error);
        }
        SUBCASE("taken by .delname")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".delname refuted"), doctest::Contains(taken), std::runtime_error);
        }
        SUBCASE("given to another node")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name c refuted"), doctest::Contains(given), std::runtime_error);
        }
        // A name in a different language belongs to the node just like any
        // other, and attempting to merge under it is refused.
        SUBCASE("merged under a name in another language")
        {
            interactive.process(".name refuted de widerlegt");
            CHECK_THROWS_WITH_AS(interactive.process(".name c de widerlegt"),
                                 doctest::Contains("Merging the two nodes is impossible because node " + id + " is 'refuted' of the engine's vocabulary, and a node of the engine's vocabulary merges with no other."),
                                 std::runtime_error);
            CHECK(z->get_name(refuted, "de", false) == "widerlegt");
        }
        SUBCASE("through the library")
        {
            const Node c = z->get_node("c");
            CHECK_THROWS_WITH_AS(z->set_name(c, "refuted", "zelph", false), doctest::Contains(given), std::runtime_error);
            CHECK_THROWS_WITH_AS(z->remove_name(refuted, "zelph"), doctest::Contains(taken), std::runtime_error);
            interactive.process(".name c de cde");
            CHECK_THROWS_WITH_AS(z->set_name("refuted", "cde", "de"), doctest::Contains(given), std::runtime_error);
            interactive.process(".lang de");
            CHECK_THROWS_WITH_AS(z->set_name("cde", "refuted", "zelph"), doctest::Contains("a node of the engine's vocabulary merges with no other"), std::runtime_error);
            interactive.process(".lang zelph");
            CHECK(z->get_name(c, "de", false) == "cde");
        }

        CHECK(z->get_node("refuted", "zelph") == refuted);
        CHECK(z->get_name(refuted, "zelph", false) == "refuted");
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) ~ refuted"});
        CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("known to be wrong"));

        // The limit is one that .help .name states.
        collector.clear();
        interactive.process(".help .name");
        CHECK(any_output_contains(collector, "stay with their nodes")); });
}

// The `.name` that assigns a name to a node in two languages creates the node
// when neither name is already in use, and the vocabulary refusal came only
// afterwards, in set_name: the node created under `undurchsichtig` remained,
// and once `opaque` existed, the identical command met that leftover node and
// was rejected once more. The refusal now precedes any creation.
TEST_CASE("name merge: a name of the engine's vocabulary refused for a node .name would create leaves no node behind")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const       z      = interactive.graph();
        const std::size_t before = node_count(collector, interactive);

        interactive.process(".lang de");
        CHECK_THROWS_WITH_AS(interactive.process(".name undurchsichtig zelph opaque"),
                             doctest::Contains("Requested name 'opaque' belongs to the engine's vocabulary in language 'zelph'. A name of the engine's vocabulary cannot be given away."),
                             std::runtime_error);
        CHECK(z->get_node("undurchsichtig", "de") == 0);
        CHECK(z->get_node("opaque", "zelph") == 0);
        CHECK(node_count(collector, interactive) == before);

        // After the vocabulary node has been established, the identical
        // command gives it a name in a different language.
        interactive.process(".lang zelph");
        interactive.process("knows ~ opaque");
        interactive.process(".lang de");
        interactive.process(".name undurchsichtig zelph opaque");
        CHECK(z->get_node("undurchsichtig", "de") == z->get_node("opaque", "zelph")); });
}

TEST_CASE("name merge: a merge that changes nothing structurally is still safe")
{
    // The control at the other end: a node with no facts at all, and a node
    // whose facts do not mention the one that disappears. Nothing may be
    // rebuilt, and nothing may be lost.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
untouched p fact
lonely1 tag t
)");
        interactive.process("lonely2 tag t");
        merge_by_name(interactive, "lonely1", "lonely2");

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "untouched p fact"));

        collector.clear();
        interactive.process("S tag O");
        CHECK(answers_contain(collector, "lonely2 tag t"));
        CHECK(collect_answers(collector).size() == 1); });
}

TEST_CASE("name merge: a repair inside a cluster is not the cluster's to roll back")
{
    // A cluster records what was CREATED while it was active, which is what
    // lets .cluster-drop promise never to destroy pre-existing knowledge. A
    // rebuilt node is not new knowledge, though -- it is the same fact under
    // the id its new components give it. Recorded as new, dropping the
    // cluster deleted a fact that predated it by any amount.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c q d
)");
        interactive.process(".cluster exp");
        merge_by_name(interactive, "a", "c");

        collector.clear();
        interactive.process("S p O");
        REQUIRE(answers_contain(collector, "c p b"));

        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p b"));

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d")); });
}

// Not a merge at all, and the point is that the command must not say it is.
//
// The cross-language `.name` is how a script binds its own vocabulary to
// imported entities -- `.name e1 wikidata Q4406616` in the disjointness dev
// scripts. Running such a script a SECOND time in a live session is ordinary:
// the alias is then already in place, so both names resolve to the same node.
// The merge branch answered that with "... are different nodes => Merging
// them", reporting a structural repair of one node with itself -- and a merge
// is precisely the operation these tests exist for, so a false report of one
// is the worst wrong answer this command has.
TEST_CASE("name merge: a cross-language alias set twice is not a merge")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".lang wikidata");
        process_lines(interactive, R"(
Q30 P279 Q10
)");
        interactive.process(".lang zelph");

        collector.clear();
        interactive.process(".name e1 wikidata Q10");
        CHECK(any_event_contains(collector, "exists, assigned name"));

        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(".name e1 wikidata Q10");
        CHECK(any_event_contains(collector, "is already the node named"));
        CHECK_FALSE(any_event_contains(collector, "Merging them"));
        CHECK_FALSE(any_event_contains(collector, "different nodes"));

        // Nothing happened to the graph, and the alias still resolves -- the
        // second call has to be a no-op, not a quieter version of the merge.
        CHECK(node_count(collector, interactive) == before);

        // And the alias does what it is for: a query written in zelph names
        // reaches the imported fact. The aliased node prints under both of its
        // names, which is why the expected line is not "Q30 P279 Q10".
        collector.clear();
        interactive.process(".name P279 wikidata P279");
        interactive.process("S \"P279\" e1");
        CHECK(answers_contain(collector, "Q30 P279 \"Q10 - e1\"")); });
}

TEST_CASE("name merge: every name writer keeps the two name maps consistent")
{
    // A removal finds the names that reference a specific node (name ->
    // node) by examining the names stored within the node (node -> name),
    // performing a single lookup per language rather than traversing the
    // whole reverse mapping. This approach remains valid only if each name
    // -> node entry is matched by a corresponding node -> name entry, hence
    // this process iterates over every writer of both maps and checks the
    // invariant following each update. A writer that violates this
    // condition results in a removed node's name resolving to an identifier
    // that no longer exists.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const auto                  check = [&](const char* writer)
    {
        INFO(writer);
        CHECK(interactive.graph()->name_maps_consistent());
    };

    interactive.process("a p b");
    check("Zelph::node");

    interactive.process(".name a de aDE");
    check(".name in another language");

    interactive.process(".name b bb");
    check(".name in the same language");

    interactive.process("(X p Y) => (Y q X)");
    interactive.process("X p Y");
    check("the display names of variables");

    interactive.process(".name a X");
    check(".name of an atom to a name a variable displays");

    interactive.process("c r d");
    interactive.graph()->set_name(interactive.graph()->get_node("c"), "aDE", "de", false);
    check("set_name without a merge");

    merge_by_name(interactive, "c", "d");
    check("a merge");

    namespace fs        = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "zelph_name_maps_consistent.bin";
    interactive.process(".save \"" + file.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + file.string() + "\"");
    check("a load");
    fs::remove(file);
}

TEST_CASE("name merge: a merge and a takeover keep the label index exact")
{
    // The carriers associated with a shared label are maintained alongside
    // the two name maps, and each writer of these maps must ensure they
    // remain synchronized: a merge moves a carrier's label to the surviving
    // node or drops it, a takeover moves the name → node entry, a rename and
    // .delname take the label from a node. A carrier missing from the index
    // corresponds to a label that ceases to resolve when its holder vanishes;
    // an excess carrier signifies a node that receives a label it does not
    // carry.
    using namespace wikidata_dump;
    const auto dump = write_lines("zelph_label_merge",
                                  {entity("Q8441", text("en", "man")),
                                   entity("Q29719014", text("en", "man")),
                                   entity("Q709369", text("en", "man"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    auto* const graph = interactive.graph();

    const auto check = [&](const char* writer)
    {
        INFO(writer);
        CHECK(graph->label_index_consistent());
        CHECK(graph->name_maps_consistent());
    };
    check("the import");

    interactive.process(".lang zelph");
    interactive.process("mine p q");
    interactive.process(".name mine en man");
    check(".name merging a node into the holder");
    CHECK(ids_of(interactive, graph->nodes_named("man", "en")) == std::vector<std::string>{"Q8441", "Q709369", "Q29719014"});

    interactive.process("theirs p q");
    interactive.process(".name theirs en husband");
    interactive.process(".name theirs wikidata Q709369");
    check(".name merging a carrier into a node with another English name");
    CHECK(ids_of(interactive, graph->nodes_named("man", "en")) == std::vector<std::string>{"Q8441", "Q29719014"});

    interactive.process("other p q");
    graph->set_name(graph->get_node("other", "zelph"), "man", "en", false);
    check("set_name taking a shared label over");
    CHECK(graph->get_node("man", "en") == graph->get_node("other", "zelph"));

    interactive.process(".lang en");
    interactive.process(".name man boy");
    check(".name renaming the holder");
    CHECK(holder_of(interactive, "man") == "Q29719014");

    interactive.process(".delname man");
    check(".delname of the holder");
    CHECK(holder_of(interactive, "man").empty());

    remove_dump(dump);
}

TEST_CASE("name merge: a merge in the same language keeps the label index exact")
{
    // .name onto a name currently held by another node in the current
    // language merges the two nodes, and the node that vanishes may carry a
    // shared label: as its holder when the name corresponds to that label, or
    // as one of several carriers when the name is its Wikidata ID. Either
    // way, the survivor assumes the vanished node's names, and with them its
    // place in the index.
    using namespace wikidata_dump;
    const auto dump = write_lines("zelph_label_same_language_merge",
                                  {entity("Q8441", text("en", "man")),
                                   entity("Q29719014", text("en", "man")),
                                   entity("Q709369", text("en", "man"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    auto* const graph = interactive.graph();

    const auto check = [&](const char* writer)
    {
        INFO(writer);
        CHECK(graph->label_index_consistent());
        CHECK(graph->name_maps_consistent());
    };

    interactive.process(".lang en");
    interactive.process("mine p q");
    const Node mine = graph->get_node("mine", "en");
    interactive.process(".name mine man");
    check(".name merging the holder into a node of the same language");
    CHECK(graph->get_node("man", "en") == mine);
    CHECK(ids_of(interactive, graph->nodes_named("man", "en")) == std::vector<std::string>{"Q8441", "Q709369", "Q29719014"});

    interactive.process(".lang wikidata");
    interactive.process("theirs p q");
    const Node theirs = graph->get_node("theirs", "wikidata");
    interactive.process(".name theirs Q709369");
    check(".name merging a carrier that does not hold the label");
    CHECK(graph->get_name(theirs, "en", false) == "man");
    CHECK(graph->get_node("man", "en") == mine);
    CHECK(graph->nodes_named("man", "en") == std::vector<Node>{mine, theirs, entity_node(interactive, "Q29719014")});

    remove_dump(dump);
}

TEST_CASE("names: a session without shared labels builds no index")
{
    // Only an imported Wikidata dump assigns a single label to several
    // nodes; all other writers either transfer a name or perform a merge. A
    // network lacking such a label maintains an empty index, as does a load
    // of such a network, which checks the two map sizes per language and
    // walks nothing when they agree.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    interactive.process(".name a de aDE");
    interactive.process("(X p Y) => (Y q X)");

    auto* graph = interactive.graph();
    CHECK(graph->label_index_consistent());
    CHECK(graph->nodes_named("a") == std::vector<Node>{graph->get_node("a")});
    CHECK(graph->nodes_named("X") == std::vector<Node>{}); // a variable does not serve as a carrier

    namespace fs        = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "zelph_no_label_index.bin";
    interactive.process(".save \"" + file.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + file.string() + "\"");

    graph = interactive.graph();
    CHECK(graph->label_index_consistent());
    CHECK(graph->nodes_named("aDE", "de") == std::vector<Node>{graph->get_node("a")});
    fs::remove(file);
}

TEST_CASE("name merge: a repair of cluster knowledge still rolls back")
{
    // The control at the other end, and the reason the membership is
    // TRANSFERRED rather than merely suppressed: where the node that was
    // rebuilt did belong to the cluster, its replacement has to belong too,
    // or the rollback would leave the fact behind.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        process_lines(interactive, R"(
a p b
c p d
)");
        merge_by_name(interactive, "a", "c");
        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty()); });
}
