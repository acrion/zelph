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

#include "network/reasoning.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace zelph::test;

TEST_CASE("clusters: drop removes cluster-created facts, keeps prior knowledge")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
keep1 relK keep2
.cluster exp
tmp1 relT tmp2
)");
        collector.clear();
        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process("X relT Y");
        CHECK_FALSE(answers_contain(collector, "tmp1 relT tmp2"));

        collector.clear();
        interactive.process("X relK Y");
        CHECK(answers_contain(collector, "keep1 relK keep2")); });
}

// A firing whose consequence nests a part writes in a scratch cluster of its
// own, which a refusal drops (test_sets.cpp, "sets: a consequence refused for
// a nested extension leaves nothing it built"). A firing that succeeds hands
// what it wrote to the active cluster, and the drop of that cluster takes it
// back, as it does every other derivation produced within the cluster.
// `c` and `d` are present before the cluster, ensuring that the cluster's
// drop does not take the derived statements along with atoms it created.
TEST_CASE("clusters: a nested consequence derived inside the cluster goes with the drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto found = collect_answers(collector);
            std::sort(found.begin(), found.end());
            return found;
        };
        process_lines(interactive, "a p b\nc rel d\n(X p Y) => ((X q Y) says (Y q X))\n.cluster t\nc p d\n");
        REQUIRE(answers("S says O").size() == 2);
        process_lines(interactive, ".cluster default\n.cluster-drop t");
        CHECK(answers("S says O") == std::vector<std::string>{"(a q b) says (b q a)"});
        CHECK(answers("S q O") == std::vector<std::string>{"a q b", "b q a"}); });
}

// The scratch of a firing records, in addition to what the firing creates,
// the pattern marks its claim revokes and the refutations it applies, and a
// successful firing transfers all three to the active cluster, whose drop
// takes them back: the rule's pattern `c q d` that the first rule claims,
// alongside the witness and the fact `?? w t`, and the refutation of the
// pattern `john sleeps nil` that the second rule writes. Each firing runs in
// the scratch, the first for its fresh variable and the second for its
// marking.
TEST_CASE("clusters: a claim and a refutation that a firing makes in its scratch go with the drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };
        SUBCASE("a claim")
        {
            process_lines(interactive, "(a p b) => (c q d)\n(X go Y) => (c q d) (Z w X)\n.cluster exp\nt go k");
            REQUIRE(answers("S q O") == std::vector<std::string>{"c q d"});
            REQUIRE(answers("S w O") == std::vector<std::string>{"?? w t"});
            process_lines(interactive, ".cluster default\n.cluster-drop exp");
            CHECK(answers("S q O").empty());
            CHECK(answers("S w O").empty());
            collector.clear();
            interactive.process(".explain (c q d)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }
        SUBCASE("a refutation")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n(X go Y) => ((john sleeps nil) ~ refuted)\n.cluster exp\nt go k");
            REQUIRE(answers("X ~ refuted") == std::vector<std::string>{"(john sleeps nil) ~ refuted"});
            process_lines(interactive, ".cluster default\n.cluster-drop exp");
            CHECK(answers("X ~ refuted").empty());
            collector.clear();
            interactive.process("mary waits nil");
            CHECK_FALSE(has_contradiction(collector));
            CHECK(answers("S sleeps O") == std::vector<std::string>{"john sleeps nil"});
        } });
}

TEST_CASE("clusters: dropping a cluster nothing outside refers to keeps the fact stores armed")
{
    // A drop removed its nodes one by one via remove_node, thereby disarming
    // the fact-path stores for the rest of the session, and disarming is
    // irreversible. A single dry run within a cluster of its own, dropped
    // afterwards, sufficed to put every later workload into the state the
    // Jacobian guard was written for, and the guard failed to detect it: its
    // workload drops nothing. A cluster whose nodes nothing outside it holds
    // is now removed just as the engine's own scratch clusters have always
    // been.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto armed = [&]
        {
            collector.clear();
            interactive.process(".fact-stores");
            return any_event_contains(collector, "Fact-path stores: on");
        };

        interactive.process("a likes b");
        REQUIRE(armed());

        SUBCASE("the drop removes what the cluster created")
        {
            process_lines(interactive, ".cluster dry\nc likes d\n.cluster default\n.cluster-drop dry");
            CHECK(armed());

            collector.clear();
            interactive.process("X likes Y");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a likes b"});
        }
        // The general path brought back a pattern that the cluster claimed;
        // the engine's scratch drops threw the revocation away, an outcome
        // no experiment can afford.
        SUBCASE("a pattern claimed inside it is marked again")
        {
            interactive.process("(e p f) => (g q h)");
            process_lines(interactive, ".cluster dry\ne p f\nc likes d\n.cluster default\n.cluster-drop dry");
            CHECK(armed());

            collector.clear();
            interactive.process("A p B");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process(".explain (g q h)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }
        // A contradiction record is a set constant, meaning the identical
        // contradiction reappears with the same identifier. Though removed
        // with the cluster, the record remained in the index of refuted
        // facts, stating "already known" regarding a contradiction that the
        // graph no longer held, and the report did not reappear.
        SUBCASE("a contradiction recorded inside it is reported again afterwards")
        {
            interactive.process("(A p B, A q B) => !");
            collector.clear();
            process_lines(interactive, ".cluster dry\nx p y\nx q y");
            REQUIRE(count_outputs_starting_with(collector, "! ⇐") >= 1);
            process_lines(interactive, ".cluster default\n.cluster-drop dry");
            CHECK(armed());

            collector.clear();
            process_lines(interactive, "x p y\nx q y");
            CHECK(count_outputs_starting_with(collector, "! ⇐") >= 1);
        }
        // The test that determines the choice between the two paths: a
        // fact written after the cluster was left holds one of its nodes,
        // and the drop must take that fact as the general path has
        // consistently done.
        SUBCASE("a cluster an outside fact refers to still takes that fact with it")
        {
            process_lines(interactive, ".cluster dry\nc likes d\n.cluster default\n(c likes d) seen-by k");
            interactive.process(".cluster-drop dry");

            collector.clear();
            interactive.process("S seen-by O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process("X likes Y");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a likes b"});
        } });
}

// The engine creates the nodes from its own vocabulary as needed, at the first
// moment one is required, and each later statement of this kind refers to the
// identical node. A cluster recorded such a node upon its creation within its
// scope, and the drop removed the node together with every statement referring
// to it -- this includes statements made outside the cluster afterwards --
// just as the SUBCASE "a cluster an outside fact refers to still takes that
// fact with it" pins for the cluster's own nodes. Now no such node is part of
// the cluster. Each SUBCASE creates one node inside the cluster first, with
// its own content nodes there, and then reuses it outside the cluster.
TEST_CASE("clusters: a vocabulary node the engine created inside a dropped cluster stays, with what refers to it outside")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        // The initial refutation during the session occurs within the cluster.
        // The drop took the refutation typed outside with it, along with the
        // node `refuted`, and `j1 waits Y` answered the denied fact as
        // asserted.
        SUBCASE("refuted")
        {
            process_lines(interactive, "x waits nil\n.cluster w1\n¬(j2 waits nil)\n.cluster default\n¬(j1 waits nil)\n.cluster-drop w1");
            CHECK(answers("j1 waits Y").empty());
            CHECK(answers("X ~ refuted") == std::vector<std::string>{"(j1 waits nil) ~ refuted"});
            CHECK_THROWS_WITH(interactive.process("j1 waits nil"), doctest::Contains("known to be wrong"));
        }
        // The first rule creates, inside the cluster, the predicate of the
        // pattern marks. The drop took along the marks of the rule written
        // outside; the session's index continued to hold them, yet following a
        // .save and a .load, both patterns of that rule answered as facts.
        SUBCASE("the rule-pattern predicate")
        {
            const auto file = std::filesystem::temp_directory_path() / "zelph_vocabulary_pattern.bin";
            process_lines(interactive, ".cluster w1\n(a p b) => (c q d)\n.cluster default\n(e r f) => (g s h)\n.cluster-drop w1");
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            std::filesystem::remove(file);
            CHECK(answers("S r O").empty());
            CHECK(answers("S s O").empty());
        }
        // The first declaration creates `opaque` within the cluster, and the
        // drop took along the declaration made outside: `says` claimed what it
        // holds.
        SUBCASE("opaque")
        {
            process_lines(interactive, ".cluster w1\nbelieves ~ opaque\n.cluster default\nsays ~ opaque\n.cluster-drop w1\nx says (a p b)");
            CHECK(answers("P ~ opaque") == std::vector<std::string>{"says ~ opaque"});
            CHECK(answers("S p O").empty());
        }
        // A path condition creates `closure` along with the node corresponding
        // to its mode. The drop took the rule written outside along with them,
        // and a drop that took only the declaration of `closure` as a relation
        // type left the rule in place but no longer matching.
        SUBCASE("closure and one-or-more")
        {
            process_lines(interactive, ".cluster w1\n(X ~ start, X p⁺ Y) => (X r Y)\n.cluster default\n(A ~ origin, A q⁺ B) => (A s B)\n.cluster-drop w1\na ~ origin\na q b\nb q c");
            const auto derived = answers("S s O");
            CHECK(std::find(derived.begin(), derived.end(), "a s b") != derived.end());
            CHECK(std::find(derived.begin(), derived.end(), "a s c") != derived.end());
        }
        SUBCASE("zero-or-more")
        {
            process_lines(interactive, ".cluster w1\n(X ~ start, X p∗ Y) => (X r Y)\n.cluster default\n(A ~ origin, A q∗ B) => (A s B)\n.cluster-drop w1\na ~ origin\na q b\nb q c");
            const auto derived = answers("S s O");
            CHECK(std::find(derived.begin(), derived.end(), "a s b") != derived.end());
            CHECK(std::find(derived.begin(), derived.end(), "a s c") != derived.end());
        }
        // `≈` creates `nn`, a net definition creates `nn-layers`; each SUBCASE
        // creates one of them within the cluster. The drop took the rule or
        // the net written outside with it, and `t1 verifiedG p1` was not
        // derived.
        SUBCASE("nn")
        {
            process_lines(interactive, R"(
.cluster w1
(A relF B, ≈fnet(A relF B)) => (A verifiedF B)
.cluster default
t1 in GIn
p1 in GOut
%(zelph/nn-connect "t1" "p1" 0.8)
gnet nn-layers <GIn GOut>
(A relG B, ≈gnet(A relG B)) => (A verifiedG B)
.cluster-drop w1
t1 relG p1
)");
            CHECK(answers("S verifiedG O") == std::vector<std::string>{"t1 verifiedG p1"});
        }
        SUBCASE("nn-layers")
        {
            process_lines(interactive, R"(
.cluster w1
fnet nn-layers <FIn FOut>
.cluster default
t1 in GIn
p1 in GOut
%(zelph/nn-connect "t1" "p1" 0.8)
gnet nn-layers <GIn GOut>
(A relG B, ≈gnet(A relG B)) => (A verifiedG B)
.cluster-drop w1
t1 relG p1
)");
            CHECK(answers("S verifiedG O") == std::vector<std::string>{"t1 verifiedG p1"});
        } });
}

// When a query is entered while a cluster remains active, it executes within
// an isolated scratch space, incorporates its retained data into the active
// cluster, and takes its text from every cluster's record before it drops
// that text. The cluster stays active and continues to record: a fact typed
// after the query becomes part of it, and dropping the cluster takes that
// fact while preserving the stores in an armed state.
TEST_CASE("clusters: a query typed in a cluster leaves it active and recording")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a likes b");
        process_lines(interactive, ".cluster dry\nX likes Y, Y likes Z\n{(X likes C)} can nil\nc likes d\n.cluster default\n.cluster-drop dry");

        collector.clear();
        interactive.process(".fact-stores");
        CHECK(any_event_contains(collector, "Fact-path stores: on"));

        collector.clear();
        interactive.process("X likes Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a likes b"}); });
}

// The query writes the declaration of a predicate it uses for the first time
// within its own scratch, and keeps the scratch in the cluster before
// discarding its text. If the cluster had not created the predicate, the
// declaration entered neither the text nor the cluster's record, and persisted
// beyond the query and the cluster's drop. When the cluster had created it,
// the declaration was placed into the record, and the drop met it as something
// constructed upon the predicate: the cascade took it, and the fact stores
// were off for the rest of the session. It now accompanies the query's text in
// both cases.
TEST_CASE("clusters: a query typed in a cluster takes back the relation-type declaration it wrote for an older predicate")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a predicate older than the cluster")
        {
            process_lines(interactive, "a likes b\n.cluster mine");
        }
        SUBCASE("a predicate the cluster created")
        {
            process_lines(interactive, ".cluster mine\na likes b");
        }
        const auto declares_b = [&]
        {
            collector.clear();
            interactive.process("P ~ ->");
            return answers_contain(collector, "b ~ ->");
        };
        const std::size_t before = node_count(collector, interactive);

        interactive.process("X b Y");
        CHECK(node_count(collector, interactive) == before);
        CHECK_FALSE(declares_b());

        process_lines(interactive, ".cluster default\n.cluster-drop mine");
        CHECK_FALSE(declares_b());
        CHECK(interactive.graph()->fact_stores_enabled());
        interactive.process("c b d");
        CHECK(declares_b()); });
}

TEST_CASE("clusters: a cluster that only added a member to an older collection leaves the collection as it was")
{
    // A set consists of its members, thus the cascade dooms a container upon
    // losing one. A drop that removed the membership `c in @{a b}` the
    // cluster had written applied that rule to a collection created before
    // the cluster existed, and took the collection and `k has @{a b}` with
    // it: prior knowledge, which a drop promises to keep. Both paths are
    // tested, since a single outside fact that holds another node within the
    // cluster determines which one runs.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
%(def s (zelph/collection "a" "b"))
%(zelph/fact "k" "has" s)
.cluster w
%(zelph/fact "c" "in" s)
x likes y
.cluster default
)");
        collector.clear();
        interactive.process("k has X");
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"k has @{a b c}"});

        SUBCASE("nothing outside refers to the cluster") {}
        SUBCASE("an outside fact refers to the cluster")
        {
            interactive.process("(x likes y) seen-by z");
        }

        interactive.process(".cluster-drop w");

        collector.clear();
        interactive.process("k has X");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k has @{a b}"});
        collector.clear();
        interactive.process("S seen-by O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("clusters: a dropped cluster walks no name map")
{
    // Removing a node previously required traversing the whole reverse name
    // map to find the entries referencing it, performed once for each node:
    // on the full Wikidata network, approximately 200 million entries per
    // node within a dropped cluster. A dry run in a scratch cluster, a
    // question's cluster, and the scratch drop of zelph/dedup-rule all drop
    // clusters, thus each incurs a cost proportional to the graph's size.
    // Removal now reads the node's own names. The counter is independent of
    // hardware, so this does not require a quiet machine.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        process_lines(interactive, R"(
keep1 relK keep2
.cluster exp
tmp1 relT tmp2
tmp3 relT tmp4
)");
        const uint64_t before = graph->name_map_scans();
        interactive.process(".cluster-drop exp");
        CHECK(graph->name_map_scans() == before);
        CHECK_THROWS_AS(interactive.process(".node tmp1"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".node tmp4"), std::runtime_error);
        interactive.process(".node keep1"); });
}

TEST_CASE("clusters: a node the drop removed is no fallback for .node, .mermaid or .explain")
{
    // If no argument is provided, these commands operate on the node linked to
    // the most recently printed statement, answer, or deduction. Removing that
    // node via a drop resulted in .node printing a record for an id that
    // denotes nothing ("Representation: ??"), .mermaid drawing a lone "??" box
    // -- the playground's graph panel after the contradiction demonstration,
    // where the final deduction occurred within a dropped cluster -- and
    // .explain answering that the fact is not asserted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("keep1 relK keep2");
        interactive.process(".cluster exp");
        interactive.process("tmp1 relT tmp2");
        collector.clear();
        interactive.process(".node");
        REQUIRE(any_output_contains(collector, "tmp1 relT tmp2"));

        interactive.process(".cluster-drop exp");
        for (const std::string command : {".node", ".mermaid", ".explain"})
        {
            CAPTURE(command);
            CHECK_THROWS_WITH_AS(interactive.process(command), doctest::Contains("no longer exists"), std::runtime_error);
        }

        // A node that is printed after the drop again functions as the
        // fallback.
        interactive.process("X relK Y");
        collector.clear();
        interactive.process(".node");
        CHECK(any_output_contains(collector, "keep1 relK keep2")); });
}

TEST_CASE("clusters: merge into default keeps facts, forgets membership")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.cluster exp
tmp1 relM tmp2
.cluster-merge exp default
)");
        collector.clear();
        interactive.process("X relM Y");
        CHECK(answers_contain(collector, "tmp1 relM tmp2"));

        collector.clear();
        interactive.process(".cluster");
        CHECK_FALSE(any_output_contains(collector, "exp")); });
}

TEST_CASE("clusters: pre-existing facts are never recorded, so drop keeps them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a relP b
.cluster exp
a relP b
.cluster-drop exp
)");
        // Re-asserting an existing fact inside the cluster must not
        // hand its nodes to the cluster: after drop it still exists.
        collector.clear();
        interactive.process("X relP Y");
        CHECK(answers_contain(collector, "a relP b")); });
}

// A name merge combines two nodes into one, and any part of it that the
// cluster did not create constitutes knowledge originating from outside the
// cluster. When the cluster had created the survivor, the drop removed it:
// given `jim` created within the cluster, `.name jim john` merged the older
// `john` into it, and the drop took the rule written about `john` before the
// cluster; with the fact `jim sleeps nil` created inside the cluster, the
// `john sleeps nil` typed before the cluster folded into that fact and went
// with the drop.
TEST_CASE("clusters: a name merge never hands what predates the cluster to its drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the survivor is a node the cluster created")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n.cluster t\njim likes ann\n.cluster default\n.name jim john\n.cluster-drop t");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(mary waits nil) => (john sleeps nil)"));
            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("the survivor is a fact the cluster created")
        {
            process_lines(interactive, "jim likes ann\njohn sleeps nil\n.cluster t\njim sleeps nil\n.cluster default\n.name jim john\n.cluster-drop t");
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"john sleeps nil"});
        } });
}

// The identical commitment along two additional routes: the merge occurs
// while the cluster responsible for creating `jim` remains active, and the
// cluster is merged into a different one before that one is dropped. In both
// cases, the drop took the survivor `john`, carrying with it the rule written
// about `john` before either cluster.
TEST_CASE("clusters: a name merge hands nothing older to the drop, made inside the cluster or before the cluster is merged into another")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the merge is made while the cluster is active")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n.cluster t\njim likes ann\n.name jim john\n.cluster default\n.cluster-drop t");
        }
        SUBCASE("the cluster is merged into another before the drop")
        {
            process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n.cluster t\njim likes ann\n.cluster u\nbob likes cake\n.cluster default\n.name jim john\n.cluster-merge t u\n.cluster-drop u");
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (john sleeps nil)"));
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty()); });
}

// A name merge may fold two facts -- each created by a distinct cluster --
// into one, yet records kept per cluster cannot express "undone once both are
// dropped". The survivor gets a single owner: the cluster responsible for
// generating the node to which the name was assigned, `jim` in
// `.name jim jon`, whose node persists through the merge. Dropping the other
// cluster keeps the survivor; dropping the owner takes it, and once both
// clusters are dropped, no trace of either fact remains. Previously, each
// cluster would read the other's fact as knowledge from outside every cluster
// and let go of the survivor, so it outlived both drops.
TEST_CASE("clusters: a name merge of two facts from two clusters leaves the survivor to the cluster of the renamed node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x sleeps nil\n.cluster t\njim sleeps nil\n.cluster u\njon sleeps nil\n.cluster default\n.name jim jon");
        const auto sleepers = [&]
        {
            collector.clear();
            interactive.process("S sleeps O");
            auto answers = collect_answers(collector);
            std::sort(answers.begin(), answers.end());
            return answers;
        };
        SUBCASE("the cluster of the renamed node dropped first")
        {
            interactive.process(".cluster-drop t");
            CHECK(sleepers() == std::vector<std::string>{"x sleeps nil"});
            interactive.process(".cluster-drop u");
        }
        SUBCASE("the other cluster dropped first")
        {
            interactive.process(".cluster-drop u");
            CHECK(sleepers() == std::vector<std::string>{"jon sleeps nil", "x sleeps nil"});
            interactive.process(".cluster-drop t");
        }
        CHECK(sleepers() == std::vector<std::string>{"x sleeps nil"});
        CHECK(interactive.graph()->get_node("jon") == 0); });
}

// The same owner for a claim: `jim sleeps nil` and `jon sleeps nil` represent
// rule patterns authored before the formation of both clusters, with each
// cluster claiming one of them. The merged statement remained claimed
// following both drops, as each cluster read the claim made by the other as
// originating from outside.
TEST_CASE("clusters: a claim of two clusters folded by a name merge goes with the cluster of the renamed node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (jim sleeps nil)\n(ann waits nil) => (jon sleeps nil)\n.cluster t\njim sleeps nil\n.cluster u\njon sleeps nil\n.cluster default\n.name jim jon");
        const auto sleepers = [&]
        {
            collector.clear();
            interactive.process("S sleeps O");
            return collect_answers(collector);
        };
        SUBCASE("the cluster of the renamed node dropped first")
        {
            interactive.process(".cluster-drop t");
            CHECK(sleepers().empty());
            interactive.process(".cluster-drop u");
        }
        SUBCASE("the other cluster dropped first")
        {
            interactive.process(".cluster-drop u");
            CHECK(sleepers() == std::vector<std::string>{"jon sleeps nil"});
            interactive.process(".cluster-drop t");
        }
        CHECK(sleepers().empty());
        collector.clear();
        interactive.process(".explain (jon sleeps nil)");
        CHECK(any_output_contains(collector, "jon sleeps nil  [rule pattern; not asserted]"));
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (jon sleeps nil)"));
        CHECK(any_output_contains(collector, "(ann waits nil) => (jon sleeps nil)")); });
}

// The same owner for a refutation: each cluster refutes one of the two
// patterns. `X ~ refuted` answered the merged statement following both
// drops, and the rule that leads to it did not derive it again.
TEST_CASE("clusters: a refutation of two clusters folded by a name merge goes with the cluster of the renamed node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (jim sleeps nil)\n(ann waits nil) => (jon sleeps nil)\n.cluster t\n¬(jim sleeps nil)\n.cluster u\n¬(jon sleeps nil)\n.cluster default\n.name jim jon");
        const auto refutations = [&]
        {
            collector.clear();
            interactive.process("X ~ refuted");
            return collect_answers(collector);
        };
        SUBCASE("the cluster of the renamed node dropped first")
        {
            interactive.process(".cluster-drop t");
            CHECK(refutations().empty());
            interactive.process(".cluster-drop u");
        }
        SUBCASE("the other cluster dropped first")
        {
            interactive.process(".cluster-drop u");
            CHECK(refutations() == std::vector<std::string>{"(jon sleeps nil) ~ refuted"});
            CHECK_THROWS_WITH(interactive.process("jon sleeps nil"), doctest::Contains("known to be wrong"));
            interactive.process(".cluster-drop t");
        }
        CHECK(refutations().empty());
        interactive.process("mary waits nil");
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"jon sleeps nil"}); });
}

// Both folded parts may originate from one cluster, and a second cluster
// only claimed one of them: `t` writes the rule using the pattern
// `jim sleeps nil` and claims `jon sleeps nil`, while `u` claims the
// pattern. The survivor stays with `t`, and the claim made by `t` stays
// regardless of which node `.name` renames: the drop of `u` takes nothing.
// As long as the claim of `into` alone decided the owner, `.name jim jon`
// gave the claim to `u`, and dropping `u` turned the fact that `t` had
// claimed back into a rule's pattern.
TEST_CASE("clusters: a claim the cluster keeping the survivor made stays when another cluster that claimed a part is dropped")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".cluster t\n(ann waits nil) => (jim sleeps nil)\njon sleeps nil\n.cluster u\njim sleeps nil\n.cluster default");
        std::string statement;
        SUBCASE("the node of the claimed pattern renamed")
        {
            interactive.process(".name jim jon");
            statement = "jon sleeps nil";
        }
        SUBCASE("the node of the claimed fact renamed")
        {
            interactive.process(".name jon jim");
            statement = "jim sleeps nil";
        }
        interactive.process(".cluster-drop u");
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{statement});
        collector.clear();
        interactive.process(".explain (" + statement + ")");
        CHECK(any_output_contains(collector, statement + "  [asserted"));
        interactive.process(".cluster-drop t");
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// The refutation counterpart of the case above: `t` writes the rule featuring
// the pattern `jim sleeps nil` and refutes `jon sleeps nil`, whereas `u`
// refutes the pattern itself. The survivor stays with `t`, and the refutation
// made by `t` persists regardless of which node `.name` renames, hence the
// drop of `u` takes nothing. Upon applying `.name jim jon`, the refutation went
// to `u`, since the owner was read from the refutation of `into`, which `u` had
// made, and the merged marking stayed with `u`, the cluster that had created
// the marking of `into`. Dropping `u` took both, and `jon sleeps nil` was
// accepted as a claim despite `t` having refuted it.
TEST_CASE("clusters: a refutation the cluster keeping the survivor made stays when another cluster that refuted a part is dropped")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".cluster t\n(ann waits nil) => (jim sleeps nil)\n¬(jon sleeps nil)\n.cluster u\n¬(jim sleeps nil)\n.cluster default");
        const auto refutations = [&]
        {
            collector.clear();
            interactive.process("X ~ refuted");
            return collect_answers(collector);
        };
        std::string statement;
        SUBCASE("the node of the pattern the other cluster refuted renamed")
        {
            interactive.process(".name jim jon");
            statement = "jon sleeps nil";
        }
        SUBCASE("the node of the fact the keeping cluster refuted renamed")
        {
            interactive.process(".name jon jim");
            statement = "jim sleeps nil";
        }
        interactive.process(".cluster-drop u");
        CHECK(refutations() == std::vector<std::string>{"(" + statement + ") ~ refuted"});
        CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("known to be wrong"));
        interactive.process(".cluster-drop t");
        CHECK(refutations().empty());
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// The early return of the case above reads who refuted each part, rather
// than who created it: `t` writes both patterns, and only `u` refutes one of
// them. The survivor stays with `t`, yet the refutation is the one `u` made,
// so the drop of `u` takes it back regardless of which node `.name` renames,
// and the rule derives the statement again. Read off the creator of the
// part, the refutation would stay with `t` and outlive the drop of `u`.
TEST_CASE("clusters: a refutation another cluster made of a part the keeping cluster created goes with that cluster's drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".cluster t\n(ann waits nil) => (jim sleeps nil)\n(bob waits nil) => (jon sleeps nil)\n.cluster u\n¬(jim sleeps nil)\n.cluster default");
        const auto refutations = [&]
        {
            collector.clear();
            interactive.process("X ~ refuted");
            return collect_answers(collector);
        };
        std::string statement;
        SUBCASE("the node of the refuted pattern renamed")
        {
            interactive.process(".name jim jon");
            statement = "jon sleeps nil";
        }
        SUBCASE("the node of the other pattern renamed")
        {
            interactive.process(".name jon jim");
            statement = "jim sleeps nil";
        }
        REQUIRE(refutations() == std::vector<std::string>{"(" + statement + ") ~ refuted"});
        interactive.process(".cluster-drop u");
        CHECK(refutations().empty());
        interactive.process("ann waits nil");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{statement}); });
}

// The first fact of a predicate establishes it as a relation type,
// `likes ~ ->`. For a predicate that predates the cluster, a fact authored
// beyond the cluster's boundaries can use the declaration as well, as in
// `c likes d`, and the drop keeps the declaration for it. Incorporated as a
// standard node within the cluster, the declaration went with the drop,
// causing `likes` to be absent from the relation types while `c likes d`
// remained intact.
TEST_CASE("clusters: the relation-type declaration of an older predicate stays when the cluster of its first fact is dropped")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "likes ~ verb\n.cluster w\na likes b\n.cluster default\nc likes d\n.cluster-drop w");
        collector.clear();
        interactive.process("P ~ ->");
        CHECK(answers_contain(collector, "likes ~ ->"));
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c likes d"}); });
}

// The first fact of the older predicate `likes` may also originate from a
// rule that fires within the cluster: `bob likes cake`, derived with `likes`
// bound to P. Its declaration `likes ~ ->` is written the same way, and the
// drop took it along, whereas `c likes d`, written outside the cluster,
// stayed.
TEST_CASE("clusters: the relation-type declaration an older predicate gets from a firing in the cluster stays after the drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "likes ~ verb\n(X ~ person, P ~ verb) => (X P cake)\n.cluster w");
        collector.clear();
        interactive.process("bob ~ person");
        REQUIRE(any_output_contains(collector, "(bob likes cake) ⇐"));
        process_lines(interactive, ".cluster default\nc likes d\n.cluster-drop w");
        collector.clear();
        interactive.process("P ~ ->");
        CHECK(answers_contain(collector, "likes ~ ->"));
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c likes d"}); });
}

// When nothing outside the cluster relies on the declaration of an older
// predicate, that declaration belongs to what the cluster wrote, and the
// drop takes it, regardless of whether a typed fact or a firing within the
// cluster wrote the predicate's first fact. Unrecorded, it persisted beyond
// every such drop, and so did the declaration of each question that a
// program posed within a cluster it subsequently dropped (refer to the case
// following the next one).
TEST_CASE("clusters: the relation-type declaration of an older predicate goes with the drop when nothing outside uses it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("likes ~ verb");
        SUBCASE("a typed fact") {}
        SUBCASE("a firing")
        {
            interactive.process("(X ~ person, P ~ verb) => (X P cake)");
        }
        const std::size_t before = node_count(collector, interactive);

        process_lines(interactive, ".cluster w\nbob ~ person\nbob likes cake\n.cluster default\n.cluster-drop w");
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "likes ~ ->"));
        CHECK(interactive.graph()->fact_stores_enabled());

        interactive.process("c likes d");
        collector.clear();
        interactive.process("P ~ ->");
        CHECK(answers_contain(collector, "likes ~ ->")); });
}

// A statement that resides outside the cluster and holds the declaration as
// a component preserves it, just as a fact relying on the predicate does.
// Taken along, the declaration sent the drop through the cascade, which took
// the statement in turn.
TEST_CASE("clusters: the relation-type declaration of an older predicate stays when a statement outside holds it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "likes ~ verb\n.cluster w\na likes b\n.cluster default\nk says (likes ~ ->)\n.cluster-drop w");
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k says (likes ~ ->)"});
        collector.clear();
        interactive.process("P ~ ->");
        CHECK(answers_contain(collector, "likes ~ ->"));
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());
        CHECK(interactive.graph()->fact_stores_enabled()); });
}

// janet.md documents how a program asks a query that results in no residual
// state: it builds and resolves the query within an isolated cluster, then
// discards that cluster. The query's pattern was the first fact of `b` as a
// predicate, and the drop took the pattern, leaving `b ~ ->`.
TEST_CASE("clusters: a question a program asks in a cluster that it drops takes back the relation-type declaration it wrote")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a likes b");
        SUBCASE("from the default cluster") {}
        SUBCASE("from a cluster of the caller's")
        {
            interactive.process(".cluster mine");
        }
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(R"js(%(do (def prev (zelph/cluster))
                                      (zelph/cluster "question")
                                      (def answers (zelph/query (zelph/fact 'X "b" 'Y)))
                                      (zelph/cluster prev)
                                      (zelph/cluster-drop "question")
                                      (zelph/out (string "ANSWERS-" (length answers)))))js");
        CHECK(any_output_contains(collector, "ANSWERS-0"));
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "b ~ ->"));
        CHECK(interactive.graph()->fact_stores_enabled());

        interactive.process("c b d");
        collector.clear();
        interactive.process("P ~ ->");
        CHECK(answers_contain(collector, "b ~ ->")); });
}

// The first fact of a predicate that the cluster created writes the
// declaration `likes ~ ->` within the cluster, and the drop takes it with the
// rest. A firing that writes its consequence into a scratch of its own
// inquired solely whether the scratch had created `likes`, causing the
// declaration to remain unrecorded: the drop subsequently encountered it as
// something built on `likes` from outside the cluster, and took the cascading
// path, thereby disarming the fact stores for the rest of the session. The
// flat consequence, written without involving the scratch, had consistently
// recorded it in the cluster.
TEST_CASE("clusters: the relation-type declaration of a predicate the cluster created goes with the drop, also from a firing's scratch")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto armed = [&]
        {
            collector.clear();
            interactive.process(".fact-stores");
            return any_event_contains(collector, "Fact-path stores: on");
        };
        SUBCASE("a consequence the firing writes in its scratch")
        {
            interactive.process("(X p Y) => (X says (a Y b))");
        }
        SUBCASE("a flat consequence")
        {
            interactive.process("(X p Y) => (a Y b)");
        }
        process_lines(interactive, ".cluster w\nk p likes");
        collector.clear();
        interactive.process("P ~ ->");
        REQUIRE(answers_contain(collector, "likes ~ ->"));

        process_lines(interactive, ".cluster default\n.cluster-drop w");
        CHECK(armed());
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "likes ~ ->")); });
}

// The same applies to the engine's named scratches kept into the cluster: a
// typed rule, a zelph/mention thunk, and a zelph/rule-text thunk run in
// `__rule`, the construction of a generated rule occurs in `__construction`,
// and a statement that takes a node by reference operates in `__claim`. When
// one of them gave `newp`, which the cluster had created, its first fact, the
// declaration remained unrecorded, since the scratch had not created `newp`.
// The drop then met it as something constructed upon `newp` and took the
// cascading path, thereby disarming the fact stores for the rest of the
// session. The declaration of a predicate older than the cluster remains
// unrecorded (as illustrated in the prior cases).
TEST_CASE("clusters: the relation-type declaration of a predicate the cluster created goes with the drop, also from a named scratch")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto armed = [&]
        {
            collector.clear();
            interactive.process(".fact-stores");
            return any_event_contains(collector, "Fact-path stores: on");
        };
        std::string lines;
        SUBCASE("a typed rule")
        {
            lines = ".cluster w\nnewp ~ thing\n(X q Y) => (X newp Y)";
        }
        SUBCASE("a zelph/mention thunk")
        {
            lines = ".cluster w\nnewp ~ thing\n" R"js(%(zelph/mention (fn [] (zelph/fact (zelph/fact 'X "q" 'Y) "=>" (zelph/fact 'X "newp" 'Y)))))js";
        }
        SUBCASE("a zelph/rule-text thunk")
        {
            lines = ".cluster w\nnewp ~ thing\n" R"js(%(zelph/rule-text (fn [] (zelph/fact (zelph/fact 'X "q" 'Y) "=>" (zelph/fact 'X "newp" 'Y)))))js";
        }
        SUBCASE("the construction of a generated rule")
        {
            lines = "(R ~ rel) => ((X q Y) => (X R Y))\n.cluster w\nnewp ~ rel";
        }
        SUBCASE("a statement that takes a node by reference")
        {
            lines = R"js(%(def m (zelph/fact "a" "p" "b")))js" "\n.cluster w\nnewp ~ thing\n,m newp y";
        }
        process_lines(interactive, lines);
        collector.clear();
        interactive.process("P ~ ->");
        REQUIRE(answers_contain(collector, "newp ~ ->"));

        process_lines(interactive, ".cluster default\n.cluster-drop w");
        CHECK(armed());
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "newp ~ ->")); });
}

// A refused firing leaves the graph unchanged from its initial state: the
// consequence operates within an isolated scratch, which the refusal drops.
// The drop took all that the firing had written, excluding solely the
// relation-type declaration. The consequence `a b c` stands as the first fact
// of `b` as a predicate, declaring `b ~ ->`, and because `b` predates the
// scratch, the declaration was never recorded anywhere and persisted, both in
// the default cluster and in a user's cluster that `b` is older than.
TEST_CASE("clusters: a refused firing takes back the relation-type declaration it wrote for an older predicate")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto armed = [&]
        {
            collector.clear();
            interactive.process(".fact-stores");
            return any_event_contains(collector, "Fact-path stores: on");
        };
        interactive.process("a p b");
        std::string after;
        SUBCASE("in the default cluster") {}
        SUBCASE("in a cluster the predicate is older than")
        {
            interactive.process(".cluster w");
            after = ".cluster default\n.cluster-drop w";
        }

        collector.clear();
        interactive.process("(X p Y) => ((X Y c) says (X in {c d}))");
        REQUIRE(any_output_contains(collector, "refused: a set constant cannot be extended"));

        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "b ~ ->"));
        collector.clear();
        interactive.process("X Y c");
        CHECK(collect_answers(collector).empty());

        process_lines(interactive, after);
        CHECK(armed());
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "b ~ ->")); });
}

// The engine discards its scratches other than a firing's in the same way: a
// zelph/mention thunk that fails keeps nothing it built, and `.explain`
// evaluates its pattern within a scratch that it discards before responding.
// Each left the declaration of an older node it had used as a predicate for
// the first time.
TEST_CASE("clusters: a dropped scratch takes back the relation-type declaration it wrote for an older predicate")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("oldp ~ thing");
        SUBCASE("a zelph/mention thunk that fails")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/mention (fn [] (zelph/fact "a" "oldp" "b") (error "stop"))))js"), doctest::Contains("stop"));
        }
        SUBCASE(".explain")
        {
            interactive.process(".explain a oldp b");
        }

        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "oldp ~ ->"));
        collector.clear();
        interactive.process(".fact-stores");
        CHECK(any_event_contains(collector, "Fact-path stores: on")); });
}

// The declaration remains in place when a fact located beyond the scratch uses
// the predicate, because that fact reads the predicate as a relation type via
// it. A predicate's facts lack its declaration solely when someone has removed
// it: a prune takes the declaration of a predicate that does not belong to the
// engine, and the facts stay. The refused firing that declares `likes` again
// preserves the declaration for `a likes b`, which relies on it.
TEST_CASE("clusters: a refused firing keeps the declaration it wrote while a fact that stays uses the predicate")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a likes b\nk p likes\n.prune-facts likes ~ ->");
        collector.clear();
        interactive.process("P ~ ->");
        REQUIRE_FALSE(answers_contain(collector, "likes ~ ->"));

        collector.clear();
        interactive.process("(X p Y) => ((X Y c) says (X in {c d}))");
        REQUIRE(any_output_contains(collector, "refused: a set constant cannot be extended"));

        collector.clear();
        interactive.process("P ~ ->");
        CHECK(answers_contain(collector, "likes ~ ->"));
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a likes b"}); });
}

// Only the declaration that the first fact of a predicate writes is the
// engine's declaration concerning an older node. A declaration typed within
// the cluster, one a rule derives there, and one a Janet program writes
// there are all equivalent to other statements, and the drop takes them.
// As long as the cluster recorded no declaration of an older node at all,
// `bob ~ ->` typed inside the cluster stayed after the drop, as did the
// other two.
TEST_CASE("clusters: a relation-type declaration stated in the cluster for an older node goes with the drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X ~ verb) => (X ~ ->)\nbob ~ thing\n.cluster w");
        SUBCASE("typed")
        {
            interactive.process("bob ~ ->");
        }
        SUBCASE("derived by a firing")
        {
            collector.clear();
            interactive.process("bob ~ verb");
            REQUIRE(any_output_contains(collector, "(bob ~ ->) ⇐"));
        }
        SUBCASE("written by a Janet program")
        {
            interactive.process(R"(%(zelph/fact "bob" "~" "->"))");
        }
        SUBCASE("nested in a statement")
        {
            // The parser writes the inner statement as a fact in its own
            // right, following the identical pathway used for a typed
            // one.
            interactive.process("(bob ~ ->) worries ann");
        }
        collector.clear();
        interactive.process("P ~ ->");
        REQUIRE(answers_contain(collector, "bob ~ ->"));
        process_lines(interactive, ".cluster default\n.cluster-drop w");
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "bob ~ ->"));
        collector.clear();
        interactive.process("X ~ thing");
        CHECK(collect_answers(collector) == std::vector<std::string>{"bob ~ thing"}); });
}

// A rule's ground consequence is written with the rule, as its pattern.
// When the cluster wrote `(mary waits nil) => (bob ~ ->)` for an older
// `bob`, that pattern went unrecorded, much like a declaration the engine
// writes, while the rule and the pattern's marking went with the drop. What
// stayed was the statement stripped of its marking, which then read as an
// axiom: `bob` was a relation type that no one had stated.
TEST_CASE("clusters: a rule's ground consequence that declares an older node goes with the drop and leaves no claim behind")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "bob ~ thing\n.cluster w\n(mary waits nil) => (bob ~ ->)\n.cluster default\n.cluster-drop w");
        collector.clear();
        interactive.process("P ~ ->");
        CHECK_FALSE(answers_contain(collector, "bob ~ ->"));
        collector.clear();
        interactive.process(".explain (bob ~ ->)");
        CHECK(any_output_contains(collector, "not asserted"));
        collector.clear();
        interactive.process("X ~ thing");
        CHECK(collect_answers(collector) == std::vector<std::string>{"bob ~ thing"}); });
}

TEST_CASE("clusters: dropping an unknown cluster is an error, not a silent no-op")
{
    // ".cluster-merge" already rejects an unknown name. ".cluster-drop"
    // reported "removed 0 node(s)" instead -- indistinguishable from the
    // honest report for a cluster that exists but is empty, so a typo read
    // as a successful rollback while the experiment kept accumulating.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH_AS(interactive.process(".cluster-drop never-created"),
                             doctest::Contains("unknown cluster"),
                             std::runtime_error);

        // An EMPTY but existing cluster still drops successfully.
        interactive.process(".cluster empty-but-real");
        collector.clear();
        interactive.process(".cluster-drop empty-but-real");
        CHECK(any_output_contains(collector, "removed 0 node(s)"));
        CHECK_FALSE(any_output_contains(collector, "unknown cluster")); });
}

TEST_CASE("clusters: a name beginning with \"__\" is refused wherever a user names a cluster")
{
    // The engine employs scratch clusters whose names begin with "__" (__rule,
    // __construction, __explain, __prune). If a user defined a cluster with
    // such a name, the engine treated it as scratch and discarded it along with
    // its contained experiment (as demonstrated in the test below). Any attempt
    // to name a cluster for activation or merging is rejected if the name
    // starts with "__", and the cluster currently active remains active,
    // preserving its contents.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("a p b");
        SUBCASE(".cluster")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".cluster __x"),
                                 doctest::Contains(".cluster: the cluster name '__x' begins with \"__\""),
                                 std::runtime_error);
        }
        SUBCASE(".cluster-merge into it")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".cluster-merge exp __x"),
                                 doctest::Contains(".cluster-merge: the cluster name '__x' begins with \"__\""),
                                 std::runtime_error);
        }
        SUBCASE("zelph/cluster")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/cluster "__x"))js"),
                              doctest::Contains("zelph/cluster: the cluster name '__x' begins with \"__\""));
        }
        collector.clear();
        interactive.process(".cluster");
        CHECK(any_output_contains(collector, "Active cluster: exp"));
        CHECK_FALSE(any_output_contains(collector, "__x"));

        interactive.process(".cluster-drop exp");
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("clusters: an experiment keeps its facts through .explain, .prune-facts and a duplicate rule")
{
    // Each of the three executions operates within an individual scratch
    // cluster and drops it afterwards. Within a user's cluster bearing the
    // scratch's name, the discard carried the experiment along: following
    // `.explain` or `.prune-facts`, `a p O` and `c q O` answered nothing, and
    // a rule typed a second time removed the facts too. The name is refused,
    // and the facts stay.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto experiment = [&](const std::string& scratch)
        {
            CHECK_THROWS_WITH_AS(interactive.process(".cluster " + scratch), doctest::Contains("reserved"), std::runtime_error);
            process_lines(interactive, "a p b\nc q d");
        };
        SUBCASE(".explain")
        {
            experiment("__explain");
            interactive.process(".explain (a p b)");
        }
        SUBCASE(".prune-facts")
        {
            experiment("__prune");
            interactive.process(".prune-facts (x y z)");
        }
        SUBCASE("a duplicate rule")
        {
            interactive.process("(X p Y) => (X r Y)");
            experiment("__rule");
            interactive.process("(X p Y) => (X r Y)");
        }
        collector.clear();
        interactive.process("a p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("c q O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"}); });
}

TEST_CASE("clusters: dropping the active cluster reports the fallback to default")
{
    // take_cluster deactivates the cluster it removes. Without saying so,
    // every node created afterwards is untracked while the user still
    // believes an experiment workspace is active.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("tmp1 relA tmp2");
        collector.clear();
        interactive.process(".cluster-drop exp");
        CHECK(any_output_contains(collector, "Active cluster: default"));

        collector.clear();
        interactive.process(".cluster");
        CHECK(any_output_contains(collector, "Active cluster: default")); });
}

// ---------------------------------------------------------------------------
// A cluster records what it CREATED, and that is what lets a drop promise
// never to destroy pre-existing knowledge. One change it makes to a
// PRE-EXISTING node has to be undone all the same: asserting a statement that
// was only a rule's ground pattern revokes that marking, and the node existed,
// so nothing was recorded and the drop undid nothing. An experiment could turn
// a rule's patterns into data permanently.
//
// The line the contract draws: a marking is the ENGINE's own bookkeeping about
// a node, not a claim anybody made. Names and merges stay outside, as before.
// ---------------------------------------------------------------------------

TEST_CASE("clusters: drop restores a rule pattern the experiment claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        // The control: both patterns are invisible to queries beforehand.
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        interactive.process(".cluster exp");
        interactive.process("a p b");

        // Inside the experiment it IS data, and the rule fires on it.
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d"));

        interactive.process(".cluster-drop exp");

        // ... and afterwards both are patterns again. The CONSEQUENCE too:
        // a ground consequence is materialized with the rule, so deriving it
        // creates no node either, and its marking was revoked the same way.
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));

        // The rule itself is untouched -- it was never the cluster's.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c q d)")); });
}

// The journal of revoked marks follows a name merge, just as the journal of
// refutations does. The merge re-creates the claimed pattern under a fresh
// identifier; keyed by the old one, the drop marked nothing, and
// `jon sleeps nil` answered as a fact even though nothing outside the cluster
// claimed it.
TEST_CASE("clusters: drop restores a rule pattern the experiment claimed after a name merge rebuilt it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(mary waits nil) => (jon sleeps nil)");

        SUBCASE("the merge re-created the pattern under a new id")
        {
            process_lines(interactive, "jim likes ann\n.cluster t\njon sleeps nil\n.cluster default\n.name jim jon\n.cluster-drop t");
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process(".explain (jon sleeps nil)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }
        // Folded into a fact asserted before the cluster, the entry goes:
        // marking that fact again would turn a statement claimed outside the
        // cluster into a pattern.
        SUBCASE("the merge folded it into a fact claimed before the cluster")
        {
            process_lines(interactive, "jim sleeps nil\n.cluster t\njon sleeps nil\n.cluster default\n.name jim jon\n.cluster-drop t");
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"jon sleeps nil"});
        }
        // Folded into a pattern from before the cluster, the survivor retains
        // the claim that the cluster made, since a claim is never relinquished
        // during a merge, and the drop takes it back: prior to the cluster,
        // both parts were patterns.
        SUBCASE("the merge folded it into a pattern from before the cluster")
        {
            process_lines(interactive, "(mary waits nil) => (jim sleeps nil)\n.cluster t\njon sleeps nil\n.cluster default\n.name jim jon\n.cluster-drop t");
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process(".explain (jon sleeps nil)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }
        // The claim is a fact the cluster created, and the pattern from before
        // the cluster folds into it. The survivor leaves the cluster, and the
        // drop takes the claim back; it formerly removed the survivor, and the
        // rule established upon it with it.
        SUBCASE("the merge folded the pattern into a fact the cluster claimed")
        {
            process_lines(interactive, "jim likes ann\n.cluster t\njim sleeps nil\n.cluster default\n.name jim jon\n.cluster-drop t");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(mary waits nil) => (jon sleeps nil)"));
            collector.clear();
            interactive.process("S sleeps O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process(".explain (jon sleeps nil)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        } });
}

TEST_CASE("clusters: a restored rule pattern is recorded in no cluster")
{
    // The drop marks again the patterns its cluster claimed, and the marking
    // it re-established was recorded within whichever cluster was active at
    // the moment of dropping: following an experiment within an inner
    // cluster, the outer one held it. Dropping the outer cluster at a later
    // stage erased the marking, yet the session continued to read the
    // pattern as unclaimed, and a .save followed by a .load read it as a
    // claim.
    const auto file = std::filesystem::temp_directory_path() / "zelph_restored_pattern.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        process_lines(interactive, ".auto-run\n(a p b) => (c q d)\n.cluster outer\n.cluster exp\na p b\n.cluster outer\n.cluster-drop exp");
        CHECK(graph->list_clusters() == std::vector<std::pair<std::string, size_t>>{{"outer", 0}});

        SUBCASE("after dropping outer and a .save/.load the pattern is still unclaimed")
        {
            process_lines(interactive, ".cluster default\n.cluster-drop outer");
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            collector.clear();
            interactive.process("A p B");
            CHECK(collect_answers(collector).empty());
        } });
    std::filesystem::remove(file);
}

TEST_CASE("clusters: a pause of the cluster recording ends with its scope, also when the scope throws")
{
    // restore_rule_patterns, rehash_dependents, and
    // Reasoning::withdraw_claims manually toggled the active cluster off and
    // on around what they re-create. A throw occurring in between left the
    // session's cluster in the off state, causing every subsequent write to
    // be recorded nowhere, so that no drop took it back. One guard object
    // pauses recording across all three, and its destructor resumes the
    // recording upon each exit.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process(".cluster exp");
        const auto paused = [&]
        {
            const zelph::network::Zelph::ClusterRecordingPause pause(*graph);
            interactive.process("a p b"); // created while paused: no cluster's
            throw std::runtime_error("inside the pause");
        };
        CHECK_THROWS_AS(paused(), std::runtime_error);

        interactive.process("c q d");
        CHECK(graph->active_cluster_name() == "exp");
        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"}); });
}

// A refutation of a fact the cluster did not generate alters no node it
// might record: it writes 0 on the fact's edge toward its predicate. The
// drop removed the marking and left the 0, thus maintaining the statement
// that a rule only wrote down as refuted throughout the session -- the
// rule's derivation was reported as a contradiction, and typing the
// statement was refused as known to be wrong. The routes are the two drop
// paths, a refutation that a run derived inside the cluster, and one
// repeated there.
TEST_CASE("clusters: dropping a cluster restores a weight a refutation inside it changed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(mary waits nil) => (john sleeps nil)");

        const auto dropped = [&]
        {
            interactive.process(".cluster-drop t");

            SUBCASE("the rule derives the statement again")
            {
                collector.clear();
                interactive.process("mary waits nil");
                CHECK_FALSE(has_contradiction(collector));
                collector.clear();
                interactive.process("S sleeps O");
                CHECK(collect_answers(collector) == std::vector<std::string>{"john sleeps nil"});
            }
            SUBCASE("the statement can be asserted")
            {
                CHECK_NOTHROW(interactive.process("john sleeps nil"));
                collector.clear();
                interactive.process("S sleeps O");
                CHECK(collect_answers(collector) == std::vector<std::string>{"john sleeps nil"});
            }
        };

        SUBCASE("nothing outside refers to the cluster")
        {
            process_lines(interactive, ".cluster t\n¬(john sleeps nil)\n.cluster default");
            dropped();
        }
        SUBCASE("an outside fact refers to the cluster")
        {
            process_lines(interactive, ".cluster t\n¬(john sleeps nil)\nk1 q k2\n.cluster default\n(k1 q k2) r s");
            dropped();
        }
        SUBCASE("a run derived the refutation")
        {
            process_lines(interactive, "(X ~ person) => ((X sleeps nil) ~ refuted)\n.cluster t\njohn ~ person\n.cluster default");
            dropped();
        }
        // run_both_modes evaluates in check mode, which additionally executes
        // the classic passes; `.semi-naive on` derives solely from the delta,
        // and the refutation such a run derives inside the cluster is taken
        // back just like any other.
        SUBCASE("a semi-naive run derived the refutation")
        {
            process_lines(interactive, ".semi-naive on\n(X ~ person) => ((X sleeps nil) ~ refuted)\n.cluster t\njohn ~ person\n.cluster default");
            dropped();
        }
        // Repeating a refutation is accepted and changes nothing; the state
        // to restore is the one before the first.
        SUBCASE("the refutation is repeated inside it")
        {
            process_lines(interactive, ".cluster t\n¬(john sleeps nil)\n¬(john sleeps nil)\n.cluster default");
            dropped();
        }
        // `.name jim john` merges the prior john into jim, and the previously
        // refuted fact is re-created under the identifier assigned by its new
        // subject. The journal continued to reference the original identifier,
        // thus the drop restored nothing, and the value 0 remained attached to
        // the new fact's edge: the statement was refused as being known to be
        // wrong.
        SUBCASE("a name merge rebuilt the fact after the refutation")
        {
            process_lines(interactive, "jim likes ann\n.cluster t\n¬(john sleeps nil)\n.cluster default\n.name jim john");
            dropped();
        }
        // The same applies to the predicate: `.name rests sleeps` merges the
        // prior sleeps into rests, the refuted fact is re-created under a new
        // predicate, and the 0 its refutation wrote shifts to the edge
        // leading to that predicate. The drop must locate both via the new
        // identifier.
        SUBCASE("a name merge of its predicate rebuilt the fact after the refutation")
        {
            process_lines(interactive, "rests ~ verb\n.cluster t\n¬(john sleeps nil)\n.cluster default\n.name rests sleeps");
            dropped();
        } });
}

// The merge folds the fact that the cluster refuted into one that was refuted
// before the cluster. Transferring the cluster's journal entry to that fact
// would let the drop restore the state the cluster found -- unrefuted -- on a
// fact that was refuted outside; the drop must leave that refutation alone.
TEST_CASE("clusters: a refutation made outside the cluster survives its drop when a name merge folded the cluster's refutation into it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(jim sleeps nil)\n.cluster t\n¬(john sleeps nil)\n.cluster default\n.name jim john\n.cluster-drop t");
        CHECK_THROWS_WITH(interactive.process("john sleeps nil"), doctest::Contains("known to be wrong"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(john sleeps nil) ~ refuted"}); });
}

// The mirror: the merge folds a refutation made outside the cluster into one
// that the cluster made. The marking the cluster created was the survivor and
// went with the drop, and the cluster's journal entry restored the fact to
// the state the cluster had found: `X ~ refuted` was empty, and
// `john sleeps nil` was accepted.
TEST_CASE("clusters: a refutation made outside the cluster survives its drop when a name merge folded it into the cluster's refutation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n(mary waits nil) => (jim sleeps nil)\n¬(john sleeps nil)\n.cluster t\n¬(jim sleeps nil)\n.cluster default\n.name jim john\n.cluster-drop t");
        CHECK_THROWS_WITH(interactive.process("john sleeps nil"), doctest::Contains("known to be wrong"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(john sleeps nil) ~ refuted"}); });
}

// The merge folds a rule's pattern, which nobody claimed, with a fact that
// the cluster refuted, in either direction, resulting in a survivor that is
// refuted. Since the refutation belongs to the cluster, the drop takes it
// back, and the rule that concludes the statement derives it. Previously,
// the merge was refused.
TEST_CASE("clusters: a refutation the cluster made goes with its drop when a name merge folded a rule's pattern nobody claimed together with it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n(ann waits nil) => (jim sleeps nil)\n.cluster t\n¬(john sleeps nil)\n.cluster default");
        std::string statement;
        SUBCASE("the pattern folded into the refuted fact")
        {
            CHECK_NOTHROW(interactive.process(".name john jim"));
            statement = "jim sleeps nil";
        }
        SUBCASE("the refuted fact folded into the pattern")
        {
            CHECK_NOTHROW(interactive.process(".name jim john"));
            statement = "john sleeps nil";
        }
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(" + statement + ") ~ refuted"});
        interactive.process(".cluster-drop t");
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        interactive.process("ann waits nil");
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{statement}); });
}

// The other way round: the cluster writes the pattern, and the refutation
// predates the cluster. A merge executed within the cluster gives the
// pattern the refutation's probability of 0, which is not a refutation the
// cluster made: the survivor holds a fact from before the cluster and is not
// part of the cluster. The drop takes the cluster's rule and keeps the
// refutation, in either direction.
TEST_CASE("clusters: a refutation from before the cluster stays after its drop when a name merge in the cluster folded the cluster's pattern into it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n¬(john sleeps nil)\n.cluster t\n(ann waits nil) => (jim sleeps nil)");
        std::string statement;
        SUBCASE("the pattern folded into the refuted fact")
        {
            CHECK_NOTHROW(interactive.process(".name john jim"));
            statement = "jim sleeps nil";
        }
        SUBCASE("the refuted fact folded into the pattern")
        {
            CHECK_NOTHROW(interactive.process(".name jim john"));
            statement = "john sleeps nil";
        }
        process_lines(interactive, ".cluster default\n.cluster-drop t");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (" + statement + ")"));
        CHECK_FALSE(any_output_contains(collector, "(ann waits nil)"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(" + statement + ") ~ refuted"});
        CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("known to be wrong")); });
}

// The refuted index is the complementary half. A marking that the cluster
// created takes its fact out of the index when the drop removes it
// (forget_refuted); one that a rule wrote as its pattern, and that the
// refutation claimed, is not removed but marked as a pattern again, and the
// fact stayed in the index. The doctest provided by the specification here
// refuted a fact that the graph claims, which no route now permits: the
// marking pattern is the means by which a pre-existing fact that the cluster
// refutes stays reachable.
TEST_CASE("clusters: dropping a cluster takes back a refutation that claimed a rule's marking pattern")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_dropped_refutation.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "((john waits nil) ~ refuted) => (mary sleeps nil)\n.cluster t\n¬(john waits nil)");
        collector.clear();
        interactive.process("S sleeps O");
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"mary sleeps nil"});
        process_lines(interactive, ".cluster default\n.cluster-drop t");

        SUBCASE("in the session") {}
        SUBCASE("after a .save and a .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
        }

        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector).empty());

        CHECK_NOTHROW(interactive.process("john waits nil"));
        collector.clear();
        interactive.process("S waits O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"}); });
    std::filesystem::remove(file);
}

// Whatever a cluster journaled regarding its refutations goes with it, no
// matter how it concludes, ensuring that a subsequent cluster sharing the
// same name restores nothing it never changed. A replacing load forgets
// every cluster, and the refutation of the loaded network then becomes the
// file's.
TEST_CASE("clusters: a later cluster of the same name takes back none of an earlier one's refutations")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_forgotten_refutation.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n.cluster t\n¬(john sleeps nil)\n.cluster default");

        SUBCASE("after its drop")
        {
            interactive.process(".cluster-drop t");
            interactive.process("¬(john sleeps nil)");
        }
        SUBCASE("after a replacing load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
        }

        process_lines(interactive, ".cluster t\n.cluster default\n.cluster-drop t");
        CHECK_THROWS_WITH(interactive.process("john sleeps nil"), doctest::Contains("known to be wrong"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(john sleeps nil) ~ refuted"}); });
    std::filesystem::remove(file);
}

TEST_CASE("clusters: merge commits the claim the experiment made")
{
    // Merging into `default` is how an experiment is KEPT, so the revocation
    // travels with the nodes instead of being rolled back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process(".cluster exp");
        interactive.process("a p b");
        interactive.process(".cluster-merge exp default");

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK_FALSE(any_output_contains(collector, "[rule pattern; not asserted]")); });
}

// The refutation accompanies the merge just as the revoked marking does:
// into `default` it is kept, which held before the drop learnt to take it
// back and must continue to hold; into any other cluster, it is that
// cluster's to take back.
TEST_CASE("clusters: merge commits a refutation the experiment made")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        process_lines(interactive, "(mary waits nil) => (john sleeps nil)\n.cluster t\n¬(john sleeps nil)");

        SUBCASE("into default")
        {
            interactive.process(".cluster-merge t default");
            CHECK_THROWS_WITH(interactive.process("john sleeps nil"), doctest::Contains("known to be wrong"));
        }
        SUBCASE("into another cluster that is dropped")
        {
            process_lines(interactive, ".cluster-merge t u\n.cluster default\n.cluster-drop u");
            CHECK_NOTHROW(interactive.process("john sleeps nil"));
        }
        // The target keeps its own entry for a fact, the earlier one: in this
        // case, `t` refuted the fact first, so the state to restore is the one
        // `t` found, not the refuted one `u` found. Using `u`'s entry, the drop
        // placed the 0 back on the fact's edge, and the statement remained
        // refused as known to be wrong.
        SUBCASE("into a cluster that refuted it first")
        {
            process_lines(interactive, ".cluster u\n¬(john sleeps nil)\n.cluster default\n.cluster-merge u t\n.cluster-drop t");
            CHECK_NOTHROW(interactive.process("john sleeps nil"));
        } });
}

TEST_CASE("clusters: a pattern the drop removed is not marked again")
{
    // The restore runs AFTER the removals and skips what is gone. It has to:
    // a rule built inside the cluster records its patterns, and asserting one
    // of them inside the same cluster revokes the marking of a node the
    // cluster itself created.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("(a p b) => (c q d)");
        interactive.process("a p b");
        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        // Nothing was resurrected by the restore either.
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}
