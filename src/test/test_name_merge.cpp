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

#include <filesystem>

using namespace zelph::test;

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

    std::size_t node_count(zelph::io::OutputCollector&        collector,
                           const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(".stat");
        for (const auto& event : collector.events())
        {
            const std::string text = normalize(event.text);
            const auto        pos  = text.find("Nodes: ");
            if (pos != std::string::npos) return std::stoul(text.substr(pos + 7));
        }
        return 0;
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
