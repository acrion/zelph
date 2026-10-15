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

#include "network/reasoning.hpp"
#include "test_helpers.hpp"
#include "wikidata_dump.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

using namespace zelph::test;
using namespace zelph::test::wikidata_dump;

// ---------------------------------------------------------------------------
// Names, languages, and the parts of facts, read from Janet without
// writing the graph.
//
// A system that operates zelph via Janet -- a front end translating its
// own language into zelph -- queries what a word names before determining
// whether to create anything, reads the session's language, works in a
// language of its own, maintains caches of node identifiers, and retrieves
// the facts provided to it. Before these bindings, any such inquiry either
// generated the subject it inquired about (zelph/resolve, zelph/fact) or
// was impossible to pose at all.
// ---------------------------------------------------------------------------

namespace
{
    std::uint64_t epoch_of(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        return std::stoull(janet_value(collector, interactive, "(zelph/epoch)"));
    }
}

TEST_CASE("janet: zelph/lookup finds an existing node and creates none")
{
    // zelph/resolve generates a node when it encounters a name it cannot
    // locate, meaning that checking whether a word refers to an entity
    // adds that word to the graph. The rule gives the name X to a
    // variable, which a command's lookup identifies: a variable is not the
    // node associated with its name, and the answer is nil.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("dog ~ animal");
    interactive.process("(X ~ dog) => (X ~ pet)");
    auto* const graph = interactive.graph();
    REQUIRE(zelph::network::Zelph::is_var(graph->get_node("X", "zelph")));

    const std::size_t before = node_count(collector, interactive);

    CHECK(janet_value(collector, interactive, R"((zelph/lookup "dog"))") == "<zelph/node dog>");
    CHECK(janet_value(collector, interactive, R"((= (zelph/lookup "dog") (zelph/resolve "dog")))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup "cat"))") == "nil");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup "dog" "de"))") == "nil");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup "X"))") == "nil");

    // A core spelling corresponds to the identical node across all
    // languages.
    CHECK(janet_value(collector, interactive, R"((= (zelph/lookup "~") (zelph/lookup "~" "de")))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup "~" "de"))") == "<zelph/node ~>");

    CHECK(node_count(collector, interactive) == before);
}

TEST_CASE("janet: a string names no rule variable in the bindings that only read")
{
    // The bindings that read without creating resolved a string to whatever
    // node the name map held for it, and a rule's variable carries its display
    // name at that location: "X" reached the variable within
    // `(X ~ dog) => (X ~ pet)`, thus the rule's pattern was "mentioned", the
    // variable possessed edges, and `.explain`'s binding explained the pattern
    // as an axiom. A string names an atom, as seen in zelph/lookup and
    // zelph/resolve; a variable is accessed via its node.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("(X ~ dog) => (X ~ pet)");
    interactive.process("rex ~ dog");
    interactive.run(true, false, false);
    REQUIRE(zelph::network::Zelph::is_var(interactive.graph()->get_node("X", "zelph")));

    const std::size_t before = node_count(collector, interactive);

    SUBCASE("zelph/mentioned")
    {
        CHECK(janet_value(collector, interactive, R"((zelph/mentioned "X" "~" "dog"))") == "false");
        CHECK(janet_value(collector, interactive, R"((zelph/mentioned "rex" "~" "dog"))") == "true");
    }
    SUBCASE("zelph/degree")
    {
        CHECK(janet_value(collector, interactive, R"((zelph/degree "X"))") == "0");
    }
    SUBCASE("zelph/explain with names")
    {
        CHECK(janet_value(collector, interactive, R"((zelph/explain "X" "~" "pet"))") == "nil");
        CHECK(janet_value(collector, interactive, R"(((zelph/explain "rex" "~" "pet") :status))") == ":derived");
    }
    SUBCASE("zelph/exists, zelph/sources, zelph/targets and a closure")
    {
        // These answered nothing about the variable before either, as they skip
        // what nobody claimed and a pattern involving a variable is never
        // claimed; the case holds them to that now that the name resolves to
        // nothing.
        CHECK(janet_value(collector, interactive, R"((zelph/exists "X" "~" "dog"))") == "false");
        CHECK(janet_value(collector, interactive, R"((length (zelph/targets "X" "~")))") == "0");
        CHECK(janet_value(collector, interactive, R"((length (zelph/sources "~" "X")))") == "0");
        CHECK(janet_value(collector, interactive, R"((length (zelph/closure "X" "~")))") == "0");
        CHECK(janet_value(collector, interactive, R"((length (zelph/targets "rex" "~")))") == "2");
    }

    CHECK(node_count(collector, interactive) == before);
}

TEST_CASE("janet: a quoted atom named like a rule's variable is found by the bindings that only read")
{
    // The other side of the case above: the string skips the variable, not the
    // name. An atom `"X"` and a rule's variable X are found under the same
    // string, and the bindings that exclusively read must provide an answer
    // regarding the atom, regardless of which of the two was written first.
    for (const bool rule_first : {true, false})
    {
        CAPTURE(rule_first);
        zelph::io::OutputCollector     collector;
        zelph::console::Interactive    interactive(collector.sink());
        const std::vector<std::string> lines = {"(X ~ dog) => (X ~ pet)", "\"X\" ~ letter"};
        interactive.process(rule_first ? lines[0] : lines[1]);
        interactive.process(rule_first ? lines[1] : lines[0]);

        CHECK(janet_value(collector, interactive, R"((zelph/exists "X" "~" "letter"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/mentioned "X" "~" "letter"))") == "true");
        CHECK(janet_value(collector, interactive, R"((length (zelph/targets "X" "~")))") == "1");
        CHECK(janet_value(collector, interactive, R"((zelph/degree "X"))") == janet_value(collector, interactive, R"((zelph/degree (zelph/lookup "X")))"));
    }
}

TEST_CASE("janet: a strict name read does not fall back")
{
    // When no name in the requested language is available, zelph/name
    // provides a name from a different language, which is right for
    // showing a node but incorrect when determining whether the language
    // names it. :strict asks the second question.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("dog ~ animal");
    interactive.process(R"(%(def dog (zelph/lookup "dog")))");

    CHECK(janet_value(collector, interactive, R"((zelph/name dog "de"))") == "\"dog\"");
    CHECK(janet_value(collector, interactive, R"((zelph/name dog "de" :strict))") == "nil");
    CHECK(janet_value(collector, interactive, R"((zelph/name dog "zelph" :strict))") == "\"dog\"");
    CHECK(janet_value(collector, interactive, R"((zelph/name dog :strict))") == "\"dog\"");

    SUBCASE("a core node answers with its spelling in every language")
    {
        CHECK(janet_value(collector, interactive, R"((zelph/name (zelph/lookup "~") "de" :strict))") == "\"~\"");
    }
    SUBCASE("an option other than :strict is refused")
    {
        CHECK_THROWS(interactive.process(R"(%(zelph/name dog "de" :strikt))"));
        CHECK_THROWS(interactive.process(R"(%(zelph/name dog "de" "en"))"));
    }
}

TEST_CASE("janet: zelph/lang reads and sets the language and returns the previous one")
{
    // A front end operating in a language of its own sets that language for
    // the duration of writing and subsequently reverts the session's
    // language to its prior state; previously, only the `.lang` command had
    // the capability to perform either action, and a handler is unable to
    // input a command.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    CHECK(janet_value(collector, interactive, "(zelph/lang)") == "\"zelph\"");
    CHECK(janet_value(collector, interactive, R"((zelph/lang "de"))") == "\"zelph\"");

    collector.clear();
    interactive.process(".lang");
    CHECK(any_output_contains(collector, "The current language is 'de'"));

    // The node that a name resolves to adheres to the language
    // configuration that has been established.
    interactive.process(R"(%(def hund (zelph/resolve "Hund")))");
    CHECK(janet_value(collector, interactive, R"((zelph/name hund "de" :strict))") == "\"Hund\"");
    CHECK(janet_value(collector, interactive, R"((zelph/name hund "zelph" :strict))") == "nil");

    CHECK(janet_value(collector, interactive, R"((zelph/lang "zelph"))") == "\"de\"");
    CHECK(janet_value(collector, interactive, "(zelph/lang)") == "\"zelph\"");

    SUBCASE("the empty language is refused")
    {
        CHECK_THROWS(interactive.process(R"(%(zelph/lang ""))"));
        CHECK(janet_value(collector, interactive, "(zelph/lang)") == "\"zelph\"");
    }
}

TEST_CASE("janet: zelph/epoch advances at .load, .new and a dump import")
{
    // A Janet cache storing node ids or labels must be aware of when their
    // significance might have ceased. graph_epoch cannot provide this
    // information: it only increases when a previously saved network is
    // loaded and replaces the current graph, and it must not increase
    // during the import of Wikidata entities into the session, since
    // consumers of graph_epoch interpret its increment as a signal of
    // replacement. As the epoch is a counter of the whole process, any
    // value read before `.new`, which also starts a new Janet VM, will
    // never be encountered again.
    namespace fs = std::filesystem;
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("bill likes ann");

    SUBCASE("a statement leaves the epoch as it is")
    {
        const std::uint64_t before = epoch_of(collector, interactive);
        interactive.process("ann likes bob");
        CHECK(epoch_of(collector, interactive) == before);
    }
    SUBCASE("a load advances the epoch")
    {
        const fs::path file = fs::temp_directory_path() / "zelph_janet_epoch.bin";
        interactive.process(".save \"" + file.string() + "\"");
        const std::uint64_t before = epoch_of(collector, interactive);
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(epoch_of(collector, interactive) > before);
        fs::remove(file);
    }
    SUBCASE(".new advances the epoch")
    {
        const std::uint64_t before = epoch_of(collector, interactive);
        interactive.process(".new");
        CHECK(epoch_of(collector, interactive) > before);
    }
    SUBCASE("a dump import advances the epoch")
    {
        // The graph contains data, thus the dump gets integrated into it:
        // no cache is read, and graph_epoch remains unchanged.
        const auto dump = write_lines("zelph_janet_epoch_dump", {entity("Q8441", text("en", "man"))});

        const std::uint64_t before       = epoch_of(collector, interactive);
        const std::uint64_t graph_before = interactive.graph()->graph_epoch();
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(epoch_of(collector, interactive) > before);
        CHECK(interactive.graph()->graph_epoch() == graph_before);
        CHECK(janet_value(collector, interactive, R"((zelph/exists "bill" "likes" "ann"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/name (zelph/lookup "man" "en") "wikidata"))") == "\"Q8441\"");

        remove_dump(dump);
    }
}

TEST_CASE("janet: zelph/lookup-all lists every carrier and creates none")
{
    // A term that connects to a Wikidata entity via its label needs every
    // item that carries the label, not merely the one the label resolves
    // to (Zelph::nodes_named). The sequence follows the ranking that elects
    // the holder.
    const auto dump = write_lines("zelph_janet_lookup_all",
                                  {entity("Q29719014", text("en", "man")),
                                   entity("Q8441", text("en", "man")),
                                   entity("Q709369", text("en", "man")),
                                   entity("Q1", text("en", "universe"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    interactive.process(".lang en");
    interactive.process("(X ~ man) => (X ~ person)");
    interactive.process(R"(%(defn ids [carriers] (map |(zelph/name $ "wikidata" :strict) carriers)))");

    const std::size_t before = node_count(collector, interactive);

    CHECK(janet_value(collector, interactive, R"((ids (zelph/lookup-all "man" "en")))") == R"(@["Q8441" "Q709369" "Q29719014"])");
    CHECK(janet_value(collector, interactive, R"((ids (zelph/lookup-all "man")))") == R"(@["Q8441" "Q709369" "Q29719014"])");
    CHECK(janet_value(collector, interactive, R"((ids (zelph/lookup-all "universe" "en")))") == R"(@["Q1"])");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup-all "nothing" "en"))") == "()");

    // Never a variable, which holds its name solely for presentation, and
    // never a core node.
    CHECK(janet_value(collector, interactive, R"((zelph/lookup-all "X" "en"))") == "()");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup-all "~" "en"))") == "()");

    CHECK(node_count(collector, interactive) == before);

    remove_dump(dump);
}

TEST_CASE("janet: zelph/lookup-all answers :incomplete after a partial load of the name sections")
{
    // A load that accesses only certain name chunks is aware of only a
    // partial set of a label's carriers, and a subset given as the answer
    // would constitute a silent error. The mark is associated with the
    // session, thus every language answers :incomplete until a load
    // replaces the graph.
    namespace fs        = std::filesystem;
    const auto     dump = write_lines("zelph_janet_lookup_partial",
                                      {entity("Q8441", text("en", "man")),
                                       entity("Q709369", text("en", "man"))});
    const fs::path file = fs::temp_directory_path() / "zelph_janet_lookup_partial_saved.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    interactive.process(".save \"" + file.string() + "\"");

    interactive.process(".load-partial \"" + file.string() + "\" nameOfNode=0 nodeOfName=0");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup-all "man" "en"))") == ":incomplete");
    CHECK(janet_value(collector, interactive, R"((zelph/lookup-all "Q8441" "wikidata"))") == ":incomplete");

    interactive.process(".load \"" + file.string() + "\"");
    CHECK(janet_value(collector, interactive, R"((length (zelph/lookup-all "man" "en")))") == "2");

    fs::remove(file);
    remove_dump(dump);
}

TEST_CASE("janet: an exact probe answers a miss without widening")
{
    // zelph/exists counts a fact involving additional objects as a
    // response, and upon failure, traverses each relation linked to the
    // subject in search of a match: on a hub within a vast graph, reaching
    // into hundreds of thousands of entries. :exact demands the fact
    // precisely as provided. Before its existence, :exact was read as an
    // object that no node carries, and answered false without a word.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b c");
    interactive.process("(r1 q r2) => (r3 q r4)");

    const std::size_t before = node_count(collector, interactive);

    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "p" "b"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "p" "b" :exact))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "p" "b" "c" :exact))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "p" "c" "b" :exact))") == "true");

    // The exact probe asks the question that its function asks: a rule's
    // pattern is mentioned, yet not claimed.
    CHECK(janet_value(collector, interactive, R"((zelph/exists "r1" "q" "r2" :exact))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/mentioned "r1" "q" "r2" :exact))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/mentioned "a" "p" "b" :exact))") == "false");

    CHECK(node_count(collector, interactive) == before);

    SUBCASE("a keyword that is no option is refused")
    {
        CHECK_THROWS(interactive.process(R"(%(zelph/exists "a" "p" "b" "c" :exakt))"));
        CHECK_THROWS(interactive.process(R"(%(zelph/exists "a" "p" :exact))"));
    }
}

TEST_CASE("janet: zelph/exists reads a statement among its arguments as a ground rule condition does")
{
    // A rule's condition reads a statement within it through the subset
    // reading: beside `(a p b c) bad k`, the rule `((a p c) bad k) => ...`
    // fires, since `a p b c` states `a p c`. The `zelph/exists` function is
    // documented to answer what a ground rule condition matches, yet it
    // widened only the top level, thereby answering false where the rule
    // fired, whether the statement served as subject or object. The embedded
    // statement is the node created by the rule's condition itself;
    // `zelph/mention` passes it to the program without claiming it, which is
    // the way a program names a statement it does not assert.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b c");
    interactive.process("(a p b c) bad k");
    interactive.process("k says (a p b c)");
    interactive.process("((a p c) bad k) => (x subject-matched y)");
    interactive.process("(k says (a p c)) => (x object-matched y)");

    // The control: both rules fire, so the conditions match.
    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "subject-matched" "y"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "object-matched" "y"))") == "true");

    interactive.process(R"(%(def apc (zelph/mention "a" "p" "c")))");
    interactive.process(R"(%(def apd (zelph/mention "a" "p" "d")))");
    const std::size_t before = node_count(collector, interactive);

    CHECK(janet_value(collector, interactive, R"((zelph/exists apc "bad" "k"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "k" "says" apc))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists apc "bad" "k" :exact))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/exists apd "bad" "k"))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "k" "says" apd))") == "false");

    CHECK(node_count(collector, interactive) == before);
}

TEST_CASE("janet: zelph/exists reads a statement as the predicate as a ground rule condition does")
{
    // The predicate position is interpreted identically: beside
    // `x (a p b c) y`, the rule `(x (a p c) y) => ...` fires, causing
    // zelph/exists to answer true for the node representing `a p c` as the
    // predicate. The condition matcher reads a concrete statement there via
    // the subset reading, and zelph/exists asks that matcher, ensuring the
    // two answers agree in this position as well.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b c");
    interactive.process("x (a p b c) y");
    interactive.process("(x (a p c) y) => (x predicate-matched y)");

    // The control: the rule fires, so the condition matches.
    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "predicate-matched" "y"))") == "true");

    interactive.process(R"(%(def apc (zelph/mention "a" "p" "c")))");
    interactive.process(R"(%(def apd (zelph/mention "a" "p" "d")))");
    const std::size_t before = node_count(collector, interactive);

    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" apc "y"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" apc "y" :exact))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" apd "y"))") == "false");

    CHECK(node_count(collector, interactive) == before);
}

// No fact containing a variable is claimed: a statement incorporating a
// variable is a pattern by construction (Zelph::is_asserted_fact). Thus, a
// question featuring a variable within its arguments has no claimed fact
// to find, and the exact probe returns false. Past the exact probe, the
// condition matcher read the question as a pattern and answered true for
// any fact that matches it, but only once a fact with additional objects
// may exist: the identical question shifted from false to true once an
// unrelated `q r s t` was written.
TEST_CASE("janet: zelph/exists answers false for a question that holds a variable")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    interactive.process("(a p b) bad k");
    interactive.process(R"(%(def v (zelph/var)))");
    interactive.process(R"(%(def pattern (zelph/mention "a" "p" v)))");

    const auto answers = [&]
    {
        return std::vector<std::string>{janet_value(collector, interactive, R"((zelph/exists "a" "p" v))"),
                                        janet_value(collector, interactive, R"((zelph/exists v "p" "b"))"),
                                        janet_value(collector, interactive, R"((zelph/exists pattern "bad" "k"))")};
    };
    const std::vector<std::string> none{"false", "false", "false"};
    CHECK(answers() == none);

    // A fact possessing additional objects, unrelated in any way to the
    // question.
    interactive.process("q r s t");
    CHECK(answers() == none);
}

TEST_CASE("janet: zelph/exists finds the wider fact beside a rule's pattern of the narrower one")
{
    // A rule written with the narrower condition leaves that statement
    // within the graph as its pattern, which remains unclaimed.
    // zelph/exists found the pattern via its hash, read it as not asserted,
    // and answered false without looking further, although the claimed
    // `a p b c` states it and the rule fired on it.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b c");
    interactive.process("(a p b) => (x fired y)");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "fired" "y"))") == "true");

    const std::size_t before = node_count(collector, interactive);
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "p" "b"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "p" "b" :exact))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/mentioned "a" "p" "b" :exact))") == "true");
    CHECK(node_count(collector, interactive) == before);
}

TEST_CASE("janet: zelph/degree counts a node's edges")
{
    // The degree of a node is the size of its adjacency, read without the
    // duplication introduced by zelph/sources. A fact points to its subject
    // and its predicate, while its subject and objects each reference it;
    // additionally, a predicate has the fact that establishes it as a
    // relation, with itself as the subject.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    interactive.process("c p d");

    const std::size_t before = node_count(collector, interactive);

    CHECK(janet_value(collector, interactive, R"([(zelph/degree "a" :in) (zelph/degree "a" :out) (zelph/degree "a")])") == "(1 1 2)");
    CHECK(janet_value(collector, interactive, R"([(zelph/degree "b" :in) (zelph/degree "b" :out) (zelph/degree "b")])") == "(0 1 1)");
    CHECK(janet_value(collector, interactive, R"([(zelph/degree "p" :in) (zelph/degree "p" :out)])") == "(3 1)");
    CHECK(janet_value(collector, interactive, R"((zelph/degree (zelph/lookup "a")))") == "2");
    CHECK(janet_value(collector, interactive, R"((zelph/degree "zz"))") == "0");

    CHECK(node_count(collector, interactive) == before);

    SUBCASE("a side other than :in and :out is refused")
    {
        CHECK_THROWS(interactive.process(R"(%(zelph/degree "a" :both))"));
    }
}

TEST_CASE("janet: zelph/parse decomposes a fact and refuses an atom")
{
    // zelph/car and zelph/cdr accept solely cons cells, and zelph/sources
    // and zelph/targets never return the parts of a fact, thus a fact a
    // program received could not be reconstructed.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(R"(%(def f (zelph/fact "john" "likes" "mary")))");

    CHECK(janet_value(collector, interactive, "(zelph/parse f)") == "(<zelph/node john> <zelph/node likes> (<zelph/node mary>))");
    CHECK(janet_value(collector, interactive, R"((zelph/parse (zelph/lookup "john")))") == "nil");
    CHECK(janet_value(collector, interactive, "(zelph/parse (zelph/var))") == "nil");
    CHECK(janet_value(collector, interactive, "(zelph/parse nil)") == "nil");

    SUBCASE("the objects come in the order of their node ids")
    {
        // The identical fact gives the identical tuple regardless of
        // the sequence in which its objects were written.
        interactive.process("x p z y");
        auto* const graph   = interactive.graph();
        const bool  y_first = graph->get_node("y", "zelph") < graph->get_node("z", "zelph");
        CHECK(janet_value(collector, interactive, R"((map zelph/name (get (zelph/parse (zelph/fact "x" "p" "y" "z")) 2)))")
              == (y_first ? R"(@["y" "z"])" : R"(@["z" "y"])"));
    }
    SUBCASE("a negated condition is the fact of its pattern being negated")
    {
        // The negation of (P), written as ¬(P), is a fact defined as
        // ((P) ~ negation): what a program encounters in the conditions of
        // a rule breaks down in the same manner as any other fact.
        interactive.process(R"(%(def pattern (zelph/fact "a" "p" "b")))");
        CHECK(janet_value(collector, interactive, R"((= (zelph/parse (zelph/negate pattern)) [pattern (zelph/lookup "~") [(zelph/lookup "negation")]]))") == "true");
    }
}
