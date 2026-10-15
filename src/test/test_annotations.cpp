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

#include "network/reasoning.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Node annotations: text fragments that a front end maintains adjacent to a
// node under a specific key (zelph/annotate, zelph/annotations,
// zelph/unannotate).
//
// A front end that translates its own language into zelph seeks to indicate
// the origin of a statement -- the sentence that asserted a fact or a rule --
// and to quote it once more later, even after a .save and a .load. This
// record cannot be a graph fact: a fact that refers to a rule node turns the
// rule off. Therefore, it is maintained beyond the graph, and must accompany
// its node through every operation involving removal, merge, cluster drop, or
// load, since node ids come back.
// ---------------------------------------------------------------------------

namespace zelph::test
{
    // Binary files from older_networks/*.bin, compiled into this
    // binary (refer to test_recipe_ids.cpp).
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

namespace
{
    namespace fs = std::filesystem;

    fs::path annotation_file(const std::string& name)
    {
        return fs::temp_directory_path() / ("zelph_annotations_" + name + ".bin");
    }

    std::vector<std::string> sorted_answers(const zelph::io::OutputCollector& collector)
    {
        std::vector<std::string> answers = collect_answers(collector);
        std::sort(answers.begin(), answers.end());
        return answers;
    }
}

TEST_CASE("annotations: a value is added once under its key, and unannotate takes one value or the whole key")
{
    // The three bindings themselves. Values keep the order in which they
    // were inserted; inserting a value into a key that already contains it
    // produces no effect and explicitly indicates this, enabling a caller
    // to annotate anew without needing to read the current state first.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    interactive.process(R"(%(def f (zelph/fact "a" "p" "b")))");

    CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == "{}");
    CHECK(janet_value(collector, interactive, R"((zelph/annotate f "source" "first"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/annotate f "source" "second"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/annotate f "source" "first"))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/annotate f "page" "7"))") == "true");

    CHECK(janet_value(collector, interactive, R"((zelph/annotations f "source"))") == R"(("first" "second"))");
    CHECK(janet_value(collector, interactive, R"((zelph/annotations f "page"))") == R"(("7"))");
    CHECK(janet_value(collector, interactive, R"((zelph/annotations f "none"))") == "()");
    CHECK(janet_value(collector, interactive, R"((length (zelph/annotations f)))") == "2");

    SUBCASE("a node given by its name")
    {
        // A name is looked up and never created: an annotation referencing
        // a name without a node is refused, and reading one yields
        // an empty answer.
        CHECK(janet_value(collector, interactive, R"((zelph/annotate "a" "note" "x"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/lookup "a")))") == R"({"note" ("x")})");
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate "never-named" "note" "x"))"),
                          doctest::Contains("zelph/annotate: \"never-named\" names no node of the graph"));
        CHECK(janet_value(collector, interactive, R"((zelph/lookup "never-named"))") == "nil");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations "never-named"))") == "{}");
    }
    SUBCASE("one value")
    {
        CHECK(janet_value(collector, interactive, R"((zelph/unannotate f "source" "first"))") == "1");
        CHECK(janet_value(collector, interactive, R"((zelph/unannotate f "source" "first"))") == "0");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations f "source"))") == R"(("second"))");
        // The last value takes its key along
        // with it.
        CHECK(janet_value(collector, interactive, R"((zelph/unannotate f "source" "second"))") == "1");
        CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == R"({"page" ("7")})");
    }
    SUBCASE("a whole key")
    {
        CHECK(janet_value(collector, interactive, R"((zelph/unannotate f "source"))") == "2");
        CHECK(janet_value(collector, interactive, R"((zelph/unannotate f "source"))") == "0");
        CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == R"({"page" ("7")})");
        CHECK(janet_value(collector, interactive, R"((zelph/unannotate f "page"))") == "1");
        CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == "{}");
    }
}

TEST_CASE("annotations: a malformed call is refused and leaves the annotations as they were")
{
    // Keys and values must be strings, and the node must be one that the
    // graph holds. Any invocation violating these rules raises an error
    // before any writing, ensuring that a caller who supplies a page number
    // as a numeric value discovers the mistake immediately, rather than later
    // learning that nothing was recorded.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
a p b
c p d
%(def f (zelph/fact "a" "p" "b"))
%(def gone (zelph/fact "c" "p" "d"))
%(zelph/annotate f "page" "7")
.prune-facts c p d
)");
    const std::string before = janet_value(collector, interactive, "(zelph/annotations f)");
    REQUIRE(before == R"({"page" ("7")})");

    SUBCASE("a key or a value that is no string")
    {
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate f "page" 8))"), doctest::Contains("expected string, got 8"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate f :page "8"))"), doctest::Contains("expected string, got :page"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate f "page" @"8"))"), doctest::Contains("expected string"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/unannotate f :page))"), doctest::Contains("expected string, got :page"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/unannotate f "page" 7))"), doctest::Contains("expected string, got 7"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotations f :page))"), doctest::Contains("expected string, got :page"));
    }
    SUBCASE("the wrong number of arguments")
    {
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate f "page"))"), doctest::Contains("arity mismatch"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate f "page" "8" "9"))"), doctest::Contains("arity mismatch"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/unannotate f))"), doctest::Contains("arity mismatch"));
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotations))"), doctest::Contains("arity mismatch"));
    }
    SUBCASE("a node value whose node was removed")
    {
        // Even after retracting a fact, a front end might still hold the node
        // value associated with it. Attempting to annotate it must not record
        // any entry under the id: if the fact is typed again, it comes back
        // under the same id and would then appear with an annotation it never
        // originally possessed.
        REQUIRE(janet_value(collector, interactive, R"((zelph/exists "c" "p" "d"))") == "false");
        CHECK_THROWS_WITH(interactive.process(R"(%(zelph/annotate gone "source" "c p d"))"), doctest::Contains("names no node of the graph"));
        CHECK(janet_value(collector, interactive, "(zelph/annotations gone)") == "{}");
        interactive.process("c p d");
        CHECK(janet_value(collector, interactive, R"((= gone (zelph/fact "c" "p" "d")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations gone)") == "{}");
    }

    CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == before);
}

TEST_CASE("annotations: an annotation is no fact: queries and rules do not see it, and an annotated rule stays in force")
{
    // When a fact refers to a rule node, it disables the rule (logic.md,
    // "Mentioning a Rule Is Not Asserting It"), hence the sentence
    // that rule came from cannot be recorded as a fact about it. An
    // annotation does not generate a node, edge, or name: the graph
    // counts the same nodes, no query finds the strings, and the
    // annotated rule continues to fire.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(X p Y) => (Y q X)
%(def r (get (first (zelph/rules)) :rule))
)");
        const std::size_t before = node_count(collector, interactive);

        process_lines(interactive, R"(
%(zelph/annotate r "source" "a sentence about p and q")
%(zelph/annotate (zelph/fact "a" "p" "b") "source" "a p b")
%(zelph/annotate (zelph/lookup "a") "source" "a")
)");
        CHECK(node_count(collector, interactive) == before);
        CHECK(janet_value(collector, interactive, R"((zelph/lookup "source"))") == "nil");
        CHECK(janet_value(collector, interactive, R"((zelph/lookup "a p b"))") == "nil");

        collector.clear();
        interactive.process("S source O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("c p d");
        collector.clear();
        interactive.process("A q B");
        CHECK(sorted_answers(collector) == std::vector<std::string>{"b q a", "d q c"});
        CHECK(janet_value(collector, interactive, "(length (zelph/rules))") == "1"); });
}

TEST_CASE("annotations: a removal erases them, so a fact re-created with the same id starts bare")
{
    // A fact is identified by its hash: when deleted and reasserted, it
    // reappears with the same identifier. An annotation surviving the
    // removal would attach the prior quotation to the new statement. Each
    // subcase represents a command that removes nodes; these reach deletion
    // via distinct routes within the engine, and the store is queried using
    // the original identifier, where a stale entry might still answer.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
a p b
c p d
%(def f (zelph/fact "a" "p" "b"))
%(zelph/annotate f "source" "the first sentence")
)");
    REQUIRE(janet_value(collector, interactive, "(zelph/annotations f)") == R"({"source" ("the first sentence")})");

    const auto refact = [&](const bool same_id)
    {
        CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == "{}");
        interactive.process("a p b");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/fact "a" "p" "b")))") == "{}");
        CHECK(janet_value(collector, interactive, R"((= f (zelph/fact "a" "p" "b")))") == (same_id ? "true" : "false"));
    };

    SUBCASE(".prune-facts of the fact")
    {
        interactive.process(".prune-facts a p b");
        refact(true);
    }
    SUBCASE(".prune-facts by a pattern")
    {
        interactive.process(".prune-facts X p Y");
        refact(true);
    }
    SUBCASE(".prune-nodes")
    {
        // The subject goes, so the fact, when typed again, is built
        // on a new node.
        interactive.process(".prune-nodes X p b");
        refact(false);
    }
    SUBCASE(".remove of the subject")
    {
        interactive.process(R"(%(zelph/annotate (zelph/lookup "a") "source" "the subject"))");
        interactive.process(R"(%(def a (zelph/lookup "a")))");
        interactive.process(".remove a");
        CHECK(janet_value(collector, interactive, "(zelph/annotations a)") == "{}");
        refact(false);
    }
    SUBCASE(".cleanup of an isolated node")
    {
        process_lines(interactive, R"(
%(def lonely (zelph/resolve "lonely"))
%(zelph/annotate lonely "source" "a word without a fact")
.cleanup
)");
        REQUIRE(janet_value(collector, interactive, R"((zelph/lookup "lonely"))") == "nil");
        CHECK(janet_value(collector, interactive, "(zelph/annotations lonely)") == "{}");
    }
    SUBCASE(".remove-rules")
    {
        process_lines(interactive, R"(
(X p Y) => (Y q X)
%(def r (get (first (zelph/rules)) :rule))
%(zelph/annotate r "source" "a rule")
.remove-rules
)");
        REQUIRE(janet_value(collector, interactive, "(length (zelph/rules))") == "0");
        CHECK(janet_value(collector, interactive, "(zelph/annotations r)") == "{}");
    }
}

TEST_CASE("annotations: a withdrawn claim keeps its node, and the node keeps them")
{
    // A ground .prune-facts of a fact that a rule names withdraws the claim
    // while keeping the node intact, as the rule's pattern. Since no action
    // eliminated the node, its annotations remain unchanged. A front end
    // that retracts the statement and ceases to quote its sentence
    // eliminates them using zelph/unannotate. This scenario is the
    // counterpart of the earlier removal case, in which the node and its
    // annotations are both discarded.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
a p b
(a p b) => (c q d)
%(def f (zelph/fact "a" "p" "b"))
%(zelph/annotate f "source" "a p b.")
.prune-facts a p b
)");
    REQUIRE(any_output_contains(collector, "Pruned 1"));
    REQUIRE(janet_value(collector, interactive, R"((zelph/exists "a" "p" "b"))") == "false");
    REQUIRE(janet_value(collector, interactive, R"((zelph/mentioned "a" "p" "b"))") == "true");
    CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == R"({"source" ("a p b.")})");
}

TEST_CASE("annotations: a name merge carries them to the surviving node, and a re-hashed fact keeps them")
{
    // Assigning a node a name that another node within the language
    // carries merges the two. The surviving node inherits
    // all that either previously carried: for each key, the survivor's
    // values appear first, followed by those of the merged node that were
    // absent. A fact from the merged node is rebuilt under the identifier
    // its new subject gives it, and its annotations accompany it,
    // ensuring no trace remains under the original identifier.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
bob likes x
john likes y
%(def old (zelph/fact "john" "likes" "y"))
%(zelph/annotate (zelph/lookup "bob") "source" "b1")
%(zelph/annotate (zelph/lookup "bob") "source" "shared")
%(zelph/annotate (zelph/lookup "john") "source" "shared")
%(zelph/annotate (zelph/lookup "john") "source" "j1")
%(zelph/annotate (zelph/lookup "john") "only-john" "j2")
%(zelph/annotate old "source" "john likes y")
)");

    SUBCASE("through .name")
    {
        // The node previously known as bob's node is the older one
        // and continues to exist, now referred to as john.
        interactive.process(".name bob john");
        REQUIRE(any_event_contains(collector, "Merging Node"));
        CHECK(janet_value(collector, interactive, R"((zelph/annotations "john" "source"))") == R"(("b1" "shared" "j1"))");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations "john" "only-john"))") == R"(("j2"))");

        CHECK(janet_value(collector, interactive, R"((= old (zelph/fact "john" "likes" "y")))") == "false");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/fact "john" "likes" "y")))") == R"({"source" ("john likes y")})");
        CHECK(janet_value(collector, interactive, "(zelph/annotations old)") == "{}");
    }
    SUBCASE("a re-hashed fact that lands on one the graph holds")
    {
        // Upon merging two subjects, hash-consing enables the consolidation
        // of two facts into a single one: john's `john likes y` is rebuilt as
        // the fact already held by bob's node. This statement now possesses
        // two origins, and both stay quotable: first, the values from the
        // fact that was previously present, followed by those values of the
        // folded fact that it lacks.
        process_lines(interactive, R"(
bob likes y
%(def held (zelph/fact "bob" "likes" "y"))
%(zelph/annotate held "source" "bob likes y")
%(zelph/annotate held "source" "shared")
%(zelph/annotate old "source" "shared")
)");
        interactive.process(".name bob john");
        REQUIRE(any_event_contains(collector, "Merging Node"));
        REQUIRE(janet_value(collector, interactive, R"((= held (zelph/fact "john" "likes" "y")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations held)") == R"({"source" ("bob likes y" "shared" "john likes y")})");
        CHECK(janet_value(collector, interactive, "(zelph/annotations old)") == "{}");
    }
    SUBCASE("through the library")
    {
        // The library overload assigns a name to a node in a second
        // language and merges two nodes when the other one holds that name
        // in that language; bob's node remains present here as well, and
        // the annotations of the nodes integrate just as they do via
        // .name.
        interactive.graph()->set_name("john", "john", "en");
        interactive.graph()->set_name("bob", "john", "en");
        REQUIRE(any_event_contains(collector, "Warning: (C) Merging Node"));
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/lookup "john" "en") "source"))") == R"(("b1" "shared" "j1"))");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/lookup "john" "en") "only-john"))") == R"(("j2"))");
    }
}

TEST_CASE("annotations: .save/.load keeps them, and a file without the section loads unchanged")
{
    // The value of a front end's record is contingent upon its persistence
    // beyond the session: .save embeds annotations as a section positioned
    // after the name sections, and a whole-file load retrieves them. A file
    // lacking this section -- every file generated by an older engine --
    // contains no annotations and loads exactly as it did previously.
    SUBCASE("a saved network brings them back")
    {
        const auto file = annotation_file("roundtrip");
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
(X p Y) => (Y q X)
%(zelph/annotate (zelph/fact "a" "p" "b") "source" "second line")
%(zelph/annotate (zelph/fact "a" "p" "b") "source" "first line")
%(zelph/annotate (zelph/fact "a" "p" "b") "quote" "Zürich, \"twice\"\nand a newline")
%(zelph/annotate (get (first (zelph/rules)) :rule) "source" "the rule")
)");
            interactive.process(".save \"" + file.string() + "\"");
        }

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/fact "a" "p" "b") "source"))") == R"(("second line" "first line"))");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (zelph/fact "a" "p" "b") "quote"))") == R"(("Z\xC3\xBCrich, \"twice\"\nand a newline"))");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations (get (first (zelph/rules)) :rule)))") == R"({"source" ("the rule")})");
        CHECK(janet_value(collector, interactive, R"((zelph/annotations "a"))") == "{}");

        // The graph remains the one previously stored: the rule
        // still fires.
        interactive.process(".auto-run");
        interactive.process("c p d");
        collector.clear();
        interactive.process("A q B");
        CHECK(sorted_answers(collector) == std::vector<std::string>{"b q a", "d q c"});
        fs::remove(file);
    }
    SUBCASE("a file an older engine wrote")
    {
        // older_networks/rule_collection.bin: `(X p Y) => (X likes @{c d})`
        // and `a p b`, saved before template identifiers, without the
        // field that counts the section.
        const auto& networks = zelph::test::older_networks();
        const auto  network  = networks.find("rule_collection");
        REQUIRE(network != networks.end());
        const auto file = annotation_file("older");
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
        }

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".stat-file \"" + file.string() + "\"");
        CHECK(any_output_contains(collector, "Total Chunks: 4"));
        CHECK_FALSE(any_output_contains(collector, "Annotation Chunks"));

        interactive.process(".load \"" + file.string() + "\"");
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a likes @{c d}"});
        CHECK(janet_value(collector, interactive, R"((zelph/annotations "a"))") == "{}");
        fs::remove(file);
    }
}

TEST_CASE("annotations: a section of more than one chunk reads back, and a load cut inside it leaves a store that agrees with itself" * doctest::test_suite("slow"))
{
    // The section is split into chunks containing a million entries each,
    // with one entry per node and key, just as in every other section. Nodes
    // are written in id order, and within each node, keys are arranged in
    // sorted order; thus, having 999,999 keys on the older node and two on
    // the newer one results in precisely one entry for the second chunk: the
    // newer node's `k2`. A million entries represents a substantial amount,
    // yet a front end that records the sentence associated with each
    // statement in a large text eventually reaches that number.
    using zelph::network::Node;
    const auto file = annotation_file("chunks");
    Node       bulk = 0;
    Node       last = 0;
    // Sufficiently extended such that the second chunk exceeds in length
    // the eight bytes extracted by the cut from the file's end, ensuring
    // the cut lands within it rather than within the first chunk.
    const std::string tail(100, 't');
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("first p second");
        auto* const graph = interactive.graph();
        bulk              = std::min(graph->get_node("first"), graph->get_node("second"));
        last              = std::max(graph->get_node("first"), graph->get_node("second"));
        std::size_t added = 0;
        for (std::size_t i = 0; i < 999'999; ++i)
        {
            const std::string digits = std::to_string(i);
            if (graph->annotate(bulk, "k" + std::string(7 - digits.size(), '0') + digits, "v")) ++added;
        }
        REQUIRE(added == 999'999);
        REQUIRE(graph->annotate(last, "k1", "first value"));
        REQUIRE(graph->annotate(last, "k2", tail));
        interactive.process(".save \"" + file.string() + "\"");
        collector.clear();
        interactive.process(".stat-file \"" + file.string() + "\"");
        REQUIRE(any_output_contains(collector, "Annotation Chunks: 2"));
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();

    SUBCASE("a whole-file load reads every chunk")
    {
        interactive.process(".load \"" + file.string() + "\"");
        using annotations = zelph::network::Zelph::node_annotations;
        CHECK(graph->annotations(last) == annotations{{"k1", {"first value"}}, {"k2", {tail}}});
        CHECK(graph->annotations(bulk).size() == 999'999);
    }
    SUBCASE("a load cut inside the second chunk")
    {
        // Upon failure of the second chunk, the first has already been
        // retrieved. Just as the graph retains the portion of the file read
        // before the failure, the store might retain those entries, but
        // whatever it keeps, a read and a write must be in alignment regarding
        // it. If a write finds a value that a read has recently indicated as
        // absent, the flag and the store's content are at odds, and in such a
        // condition, a removal leaves the entries of its node behind,
        // awaiting the next fact created with the same id.
        const auto cut = annotation_file("chunks-cut");
        fs::copy_file(file, cut, fs::copy_options::overwrite_existing);
        fs::resize_file(cut, fs::file_size(cut) - 8);
        CHECK_THROWS_AS(interactive.process(".load \"" + cut.string() + "\""), std::runtime_error);
        REQUIRE(std::max(graph->get_node("first"), graph->get_node("second")) == last);

        const auto seen = graph->annotations(last);
        CHECK(graph->annotate(last, "k1", "first value") == (seen.find("k1") == seen.end()));
        fs::remove(cut);
    }

    fs::remove(file);
}

TEST_CASE("annotations: a dropped cluster takes the annotations of its nodes")
{
    // A cluster keeps track of the nodes it created, and a drop removes
    // them: their annotations are taken along, meaning `a p b`, when
    // re-created after the drop using the same identifier, begins without
    // annotations. Both drop routes are accounted for; the specific one
    // executed hinges on whether a fact external to the cluster holds one of
    // its nodes.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    // `a`, `p` and `b` are present before the cluster, such that it creates
    // the fact alone and the fact returns under its identifier.
    process_lines(interactive, R"(
a p c
d p b
older p q
.cluster exp
a p b
%(def f (zelph/fact "a" "p" "b"))
%(zelph/annotate f "source" "made in the cluster")
%(zelph/annotate (zelph/lookup "older") "source" "added in the cluster")
.cluster default
)");

    SUBCASE("nothing outside refers to the cluster")
    {
        interactive.process(".cluster-drop exp");
        collector.clear();
        interactive.process(".fact-stores");
        REQUIRE(any_event_contains(collector, "Fact-path stores: on")); // the path that keeps them armed
    }
    SUBCASE("an outside fact refers to the cluster")
    {
        interactive.process("(a p b) seen-by k");
        interactive.process(R"(%(def holder (zelph/fact (zelph/fact "a" "p" "b") "seen-by" "k")))");
        interactive.process(R"(%(zelph/annotate holder "source" "the holder"))");
        interactive.process(".cluster-drop exp");
        // The act of dropping takes the holder along
        // with the fact it holds.
        collector.clear();
        interactive.process("S seen-by O");
        REQUIRE(collect_answers(collector).empty());
        CHECK(janet_value(collector, interactive, "(zelph/annotations holder)") == "{}");
    }

    CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == "{}");
    interactive.process("a p b");
    CHECK(janet_value(collector, interactive, R"((= f (zelph/fact "a" "p" "b")))") == "true");
    CHECK(janet_value(collector, interactive, "(zelph/annotations f)") == "{}");

    // What the cluster added to a node it did not generate remains
    // unrecorded, much like a neural weight: the node stays, and
    // so does the annotation.
    CHECK(janet_value(collector, interactive, R"((zelph/annotations "older"))") == R"({"source" ("added in the cluster")})");
}
