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
#include <fstream>
#include <sstream>
#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .stat-file and .index-file off the happy path.
//
// Both are the entry point of the sharding pipeline, so what they say about a
// file is believed by everything downstream. They used to answer a file that
// is not a .bin, or is empty, with the raw kj exception -- a C++ source
// location and a hex stack trace -- and .stat-file accepted any header,
// including one declaring more chunks than the file has bytes.
//
// The division of labour between the two is deliberate and now documented:
// .stat-file reads the header only, which is what makes it instant on an
// 88 GB file and also means it cannot see a file truncated after the header.
// .index-file reads every chunk and is the one that notices.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    struct Fixtures
    {
        fs::path root;
        fs::path good;      // a saved two-fact network
        fs::path empty;     // a saved network with nothing but the core nodes
        fs::path truncated; // good, cut after the header
        fs::path tiny;      // good, cut inside the header's own claims
        fs::path garbage;   // not a .bin at all
        fs::path nothing;   // zero bytes
    };

    void copy_prefix(const fs::path& from, const fs::path& to, std::streamsize bytes)
    {
        std::ifstream     in(from, std::ios::binary);
        std::vector<char> buffer(static_cast<size_t>(bytes));
        in.read(buffer.data(), bytes);
        std::ofstream(to, std::ios::binary).write(buffer.data(), in.gcount());
    }

    Fixtures build()
    {
        Fixtures f;
        f.root = fs::temp_directory_path() / "zelph_bin_inspection";
        fs::remove_all(f.root);
        fs::create_directories(f.root);

        f.good      = f.root / "good.bin";
        f.empty     = f.root / "empty.bin";
        f.truncated = f.root / "truncated.bin";
        f.tiny      = f.root / "tiny.bin";
        f.garbage   = f.root / "garbage.bin";
        f.nothing   = f.root / "nothing.bin";

        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
c p d
)");
            interactive.process(".save \"" + f.good.string() + "\"");
            interactive.process(".new");
            interactive.process(".save \"" + f.empty.string() + "\"");
        }

        copy_prefix(f.good, f.truncated, 200);
        copy_prefix(f.good, f.tiny, 40);
        std::ofstream(f.garbage, std::ios::binary) << "hello, this is not a serialized network";
        std::ofstream nothing(f.nothing, std::ios::binary); // created empty on purpose
        nothing.close();

        return f;
    }
}

TEST_CASE("bin inspection: a file that is not a network is refused in words")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    for (const auto& bad : {f.garbage, f.nothing})
    {
        CHECK_THROWS_AS(interactive.process(".stat-file \"" + bad.string() + "\""), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".index-file \"" + bad.string() + "\" \""
                                            + (f.root / "out.json").string() + "\""),
                        std::runtime_error);

        // The message has to name the file and say what is wrong with it. The
        // kj detail may follow, the stack trace may not.
        try
        {
            interactive.process(".stat-file \"" + bad.string() + "\"");
            FAIL("expected .stat-file to refuse " << bad.string());
        }
        catch (const std::runtime_error& e)
        {
            const std::string message(e.what());
            CHECK(message.find(bad.filename().string()) != std::string::npos);
            CHECK(message.find("zelph .bin") != std::string::npos);
            CHECK(message.find("stack:") == std::string::npos);
        }
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a header claiming more than the file holds is refused")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // 40 bytes hold the header, whose four chunk counts cannot possibly be
    // backed by what is left. That is decidable without reading a chunk.
    try
    {
        interactive.process(".stat-file \"" + f.tiny.string() + "\"");
        FAIL("expected .stat-file to refuse a header that does not fit");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("chunks") != std::string::npos);
        CHECK(message.find("truncated") != std::string::npos);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: the header report and the chunk walk divide the work")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Happy path: four chunks, and the report says where the numbers come from.
    collector.clear();
    interactive.process(".stat-file \"" + f.good.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(any_output_contains(collector, "declared by the header"));

    // A network with only the core nodes has no names to write, so two of the
    // four sections are empty -- a shape the pipeline has to accept.
    collector.clear();
    interactive.process(".stat-file \"" + f.empty.string() + "\"");
    CHECK(any_output_contains(collector, "Name-of-Node Chunks: 0"));
    CHECK(any_output_contains(collector, "Node-of-Name Chunks: 0"));

    // Truncated after the header: .stat-file reports what the header says,
    // because it never looks further. This is the documented division of
    // labour, not an oversight -- pinned so that a future change to either
    // command has to be deliberate.
    collector.clear();
    interactive.process(".stat-file \"" + f.truncated.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));

    // ...and .index-file, which reads every chunk, is the one that notices.
    CHECK_THROWS_AS(interactive.process(".index-file \"" + f.truncated.string() + "\" \""
                                        + (f.root / "out.json").string() + "\""),
                    std::runtime_error);

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: the annotation section is counted")
{
    // Node annotations constitute a fifth section following the name
    // sections, appearing solely when the network includes any. Both
    // commands convey this information: the header's count, and the chunk
    // walk's place for the section, which runs from the last name chunk to
    // the end of the file. For every other file, they continue to express
    // the same details as before the section's existence, ensuring that
    // published networks and the indexes made from them remain unchanged.
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const fs::path annotated = f.root / "annotated.bin";
    process_lines(interactive, R"(
a p b
c p d
%(zelph/annotate (zelph/fact "a" "p" "b") "source" "a sentence")
)");
    interactive.process(".save \"" + annotated.string() + "\"");

    collector.clear();
    interactive.process(".stat-file \"" + annotated.string() + "\"");
    CHECK(any_output_contains(collector, "Annotation Chunks: 1"));
    CHECK(any_output_contains(collector, "Total Chunks: 5"));

    const fs::path json = f.root / "annotated.json";
    interactive.process(".index-file \"" + annotated.string() + "\" \"" + json.string() + "\"");
    std::string index;
    {
        std::ifstream     in(json);
        std::stringstream text;
        text << in.rdbuf();
        index = text.str();
    }
    const auto number_after = [&index](const std::size_t from, const std::string& key)
    {
        const std::size_t at = index.find("\"" + key + "\":", from);
        REQUIRE(at != std::string::npos);
        return std::stoull(index.substr(at + key.size() + 3));
    };
    const std::size_t section = index.find("\"annotations\": [");
    REQUIRE(section != std::string::npos);
    CHECK(number_after(section, "chunkIndex") == 0);
    CHECK(number_after(section, "offset") + number_after(section, "length") == fs::file_size(annotated));
    const std::size_t names = index.find("\"nodeOfName\": [");
    REQUIRE(names != std::string::npos);
    CHECK(number_after(names, "offset") + number_after(names, "length") == number_after(section, "offset"));

    SUBCASE("a file without annotations reports no section")
    {
        collector.clear();
        interactive.process(".stat-file \"" + f.good.string() + "\"");
        CHECK(any_output_contains(collector, "Total Chunks: 4"));
        CHECK_FALSE(any_output_contains(collector, "Annotation Chunks"));

        interactive.process(".index-file \"" + f.good.string() + "\" \"" + json.string() + "\"");
        std::ifstream     in(json);
        std::stringstream text;
        text << in.rdbuf();
        CHECK(text.str().find("annotations") == std::string::npos);
    }
    SUBCASE("a file cut inside the section is noticed by the walk and by a load")
    {
        // The header continues to assert the section, which .stat-file
        // believes; the commands that read the chunks detect its
        // incompleteness.
        const fs::path cut = f.root / "annotated-cut.bin";
        copy_prefix(annotated, cut, static_cast<std::streamsize>(fs::file_size(annotated) - 8));
        collector.clear();
        interactive.process(".stat-file \"" + cut.string() + "\"");
        CHECK(any_output_contains(collector, "Total Chunks: 5"));
        CHECK_THROWS_AS(interactive.process(".index-file \"" + cut.string() + "\" \"" + json.string() + "\""), std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process(".load \"" + cut.string() + "\""), doctest::Contains("zelph .bin"), std::runtime_error);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a failed load says what happened to the network")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Upon reading the file's header, a load supersedes the graph, meaning
    // that a file interrupted mid-process leaves part of its content in the
    // graph and nothing of the session. Previously, the message was the kj
    // backtrace and conveyed no information regarding this; the recovery path
    // is .new, and the message now indicates this explicitly.
    try
    {
        interactive.process(".load \"" + f.truncated.string() + "\"");
        FAIL("expected .load to refuse a truncated file");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("zelph .bin") != std::string::npos);
        CHECK(message.find(".new") != std::string::npos);
        CHECK(message.find("stack:") == std::string::npos);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a file that is not a network leaves the session's graph as it was")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");

    // A file that is not a network encounters failure at the header stage,
    // before any interaction with the graph: the session keeps its graph, and
    // the message must refrain from redirecting the user to .new for a network
    // that remains present.
    try
    {
        interactive.process(".load \"" + f.garbage.string() + "\"");
        FAIL("expected .load to refuse a file that is not a network");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("zelph .bin") != std::string::npos);
        CHECK(message.find(".new") == std::string::npos);
    }

    collector.clear();
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    fs::remove_all(f.root);
}

// A complete-file .load previously integrated the file into the session
// incrementally, node by node, due to the chunk loaders' clears being
// positioned beneath a macro with no definition. A fact within the session,
// whose ids the file did not reference, remained intact; a session name
// continued to point to whichever file node took its identifier (`bill` was
// reassigned to the file's `john`, and a typed `bill likes Y` queried john);
// and the file's variable counter assigned an identifier that a session
// variable still retained, causing the first query involving a variable to
// fail with "Network::var: Node 18446744073709551615 already in use". A
// cluster recorded before the load still retained its listed identifiers, and
// dropping it removed the loaded nodes that had reused those identifiers. A
// load now starts from a blank graph and discards any knowledge the session
// kept about the old one; the subcases take each piece in sequence, mirroring
// how a session reaches it.
TEST_CASE("load: a network file replaces the session's graph")
{
    const fs::path plain     = fs::temp_directory_path() / "zelph_load_replaces.bin";
    const fs::path rules     = fs::temp_directory_path() / "zelph_load_replaces_rules.bin";
    const fs::path self_fact = fs::temp_directory_path() / "zelph_load_replaces_self_fact.bin";
    const fs::path list      = fs::temp_directory_path() / "zelph_load_replaces_list.bin";
    const fs::path term      = fs::temp_directory_path() / "zelph_load_replaces_term.bin";
    const auto     save      = [](const fs::path& file, const std::string& lines)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, lines);
        interactive.process(".save \"" + file.string() + "\"");
    };
    save(plain, "john ~ man\njohn likes mary");
    // A rule, and a fact that remains unapplied: auto-run is off upon the
    // arrival of `john ~ man`, meaning the file holds no `john ~ mortal`.
    save(rules, "(X ~ man) => (X ~ mortal)\n.auto-run\njohn ~ man");
    // Three files in which the nodes take the ids of nodes registered by a
    // subcase for display, each built in the same order as the session's.
    save(self_fact, "x likes x");
    save(list, "x has <a b>");
    save(term, "(a andthen b) unless c");

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const auto                  session = [&interactive]
    {
        process_lines(interactive, R"(
bill ~ man
bill likes ann
p1 q1 r1
p2 q2 r2
p3 q3 r3
(X ~ man) => (X ~ mortal)
)");
    };

    SUBCASE("the session's facts, names and rules go")
    {
        session();
        interactive.process(".load \"" + plain.string() + "\"");

        collector.clear();
        interactive.process("X ~ man");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john ~ man"});
        CHECK_THROWS_WITH_AS(interactive.process(".node bill"), doctest::Contains("No node found with name 'bill'"), std::runtime_error);
        collector.clear();
        interactive.process("p1 q1 X");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found."));
    }
    SUBCASE("a variable query after the load answers, whatever variables the session made")
    {
        // The reported reproduction: a query leaves its variables within the
        // graph, and the first variable of a query following the load collided
        // with one of those.
        interactive.process("X p Y");
        interactive.process(".load \"" + plain.string() + "\"");
        collector.clear();
        CHECK_NOTHROW(interactive.process("X ~ man"));
        CHECK(answers_contain(collector, "john ~ man"));
    }
    SUBCASE("a variable a Janet block named before the load is a new one after it")
    {
        // Within a single Janet block, a symbol names a single variable.
        // Following the load, the file's rule assigns the variable's
        // identifier to its own `X`, and the block's next `'X` took that node
        // over. The load makes the symbol name a new variable.
        collector.clear();
        interactive.process(R"(%(do (def before (zelph/fact 'X "in" "nil")) (zelph/load ")" + rules.generic_string()
                            + R"(") (zelph/out (string/format "same=%j" (= before (zelph/fact 'X "in" "nil"))))))");
        CHECK(any_output_contains(collector, "same=false"));
    }
    SUBCASE("the node printed last is forgotten")
    {
        // The `.node` directive, when used without a parameter, displays the
        // most recently printed node. The entries `bill ~ woman` and the
        // file's `john ~ man` are built from nodes sharing identical
        // identifiers, hence the prior identifier refers to the fact stored in
        // the file, which has never been output.
        interactive.process("bill ~ woman");
        interactive.process(".load \"" + plain.string() + "\"");
        CHECK_THROWS_WITH_AS(interactive.process(".node"), doctest::Contains("no previous output node"), std::runtime_error);
    }
    SUBCASE("a predicate that only the file declares is one")
    {
        // Predicate detection reads a cached collection of relation types,
        // while the session constructs its own set based on its `p`. Preserved
        // throughout the load, this collection concealed each fact associated
        // with a predicate that only the file declares.
        interactive.process("a p b");
        interactive.process(".load \"" + plain.string() + "\"");
        collector.clear();
        interactive.process("X likes Y");
        CHECK(answers_contain(collector, "john likes mary"));
    }
    SUBCASE("the input focus is forgotten")
    {
        // The deduction filter reveals the logical consequences of the input.
        // Since `bill` and the file's `john` share an id, a focus maintained
        // throughout the load showed `john ~ mortal` as though it derived from
        // the session's input.
        process_lines(interactive, "(X ~ man) => (X ~ mortal)\nbill ~ woman");
        interactive.process(".load \"" + rules.string() + "\"");
        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(any_output_contains(collector, "(john ~ mortal) ⇐"));
        collector.clear();
        interactive.process("X ~ mortal");
        CHECK(answers_contain(collector, "john ~ mortal"));
    }
    SUBCASE("a cluster recorded before the load is forgotten")
    {
        // c1 contains the session's nodes, while c2 is active upon loading.
        // Both go: none of their identifiers names what they recorded any
        // more, and dropping c1 caused the loaded nodes to be dropped.
        process_lines(interactive, ".cluster c1\nbill ~ man\nbill likes ann\n.cluster c2\nann ~ woman");
        interactive.process(".load \"" + plain.string() + "\"");

        collector.clear();
        interactive.process(R"(%(zelph/out (string/format "active=%j clusters=%j" (zelph/cluster) (zelph/clusters))))");
        CHECK(any_output_contains(collector, "active=nil clusters=@[]"));
        CHECK_THROWS_WITH_AS(interactive.process(".cluster-drop c1"), doctest::Contains("unknown cluster 'c1'"), std::runtime_error);
        collector.clear();
        interactive.process("X likes Y");
        CHECK(answers_contain(collector, "john likes mary"));
    }
    SUBCASE("the Janet state and a registered keyword survive, and auto-run is off")
    {
        // What lies outside the graph remains after a load: Janet definitions,
        // a registered keyword, and the session's settings. Auto-run is
        // disabled, as a load has always done.
        session();
        interactive.process("%(def kept @{:k 1})");
        interactive.process(R"(%(zelph/register-keyword "vk" (fn [text] (zelph/out (string "VK got: " text)) nil)))");
        interactive.process(".lang en");
        interactive.process(".load \"" + plain.string() + "\"");
        CHECK_FALSE(interactive.is_auto_run_active());

        collector.clear();
        interactive.process(R"(%(zelph/out (string/format "kept=%j" kept)))");
        CHECK(any_output_contains(collector, "kept=@{:k 1}"));
        interactive.process("vk hello.");
        interactive.process("");
        CHECK(any_output_contains(collector, "VK got: hello."));
        interactive.process(".lang");
        CHECK(any_output_contains(collector, "The current language is 'en'"));
    }
    SUBCASE("a small file says that the load switched auto-run off and why, without calling itself large")
    {
        // A load switches auto-run off, regardless of the content it loads,
        // and it indicated this with the message "due to loading a large
        // dataset" even when the file contained just two facts. No mechanism
        // evaluates the size of a load, hence the message explains the
        // occurrence and the rationale applicable to every load.
        collector.clear();
        interactive.process(".load \"" + plain.string() + "\"");
        CHECK(any_output_contains(collector, "Auto-run has been disabled, as every .load disables it"));
        CHECK_FALSE(any_event_contains(collector, "large dataset"));
        CHECK_FALSE(interactive.is_auto_run_active());
    }
    SUBCASE("a self-fact whose predicate shares an id with a verbose one is sugar")
    {
        // zelph/no-selffact-sugar records a predicate via its identifier. The
        // session's `loves` and the file's `likes` are built in identical
        // sequence and share it, and the registration kept through the load
        // printed the file's self-fact entirely, as though `likes` functioned
        // as a term-forming operator.
        process_lines(interactive, "y loves y\n%(zelph/no-selffact-sugar \"loves\")");
        interactive.process(".load \"" + self_fact.string() + "\"");

        collector.clear();
        interactive.process("X likes Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{":likes x"});
    }
    SUBCASE("a list of nodes that share ids with the digits is a list")
    {
        // zelph/set-number-digits records the digits indexed by id, and a list
        // composed solely of these digits outputs as a numeric value. The
        // file's `a` and `b` take the ids corresponding to the session's
        // digits `c` and `d`, and the file's list printed as &2.
        process_lines(interactive, "y owns <c d>\n%(zelph/set-number-digits [\"c\" \"d\"])");
        interactive.process(".load \"" + list.string() + "\"");

        collector.clear();
        interactive.process("X has Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x has <a b>"});
    }
    SUBCASE("a display scheme goes with the operators it claimed")
    {
        // The scheme claims `r` and `s` by identifier, the file's `andthen`
        // and `unless` take those ids, and the file's term printed in the
        // scheme's notation. The claim kept through the load additionally
        // rejected the module's own registration of `andthen` upon subsequent
        // module execution ("already registered in scheme 'policy'"); the
        // scheme is likewise discarded, enabling the module to register it
        // anew.
        process_lines(interactive, "(p r q) s w\n"
                                   "%(do (zelph/register-display-scheme \"policy\" \"[[\" \"]]\" "
                                   "{:name-first \"abcdefghijklmnopqrstuvwxyz\" :name-chars \"abcdefghijklmnopqrstuvwxyz\"}) "
                                   "(zelph/set-infix-display \"policy\" [[\"r\" 20 :left] [\"s\" 10 :left]]))");
        interactive.process(".load \"" + term.string() + "\"");

        collector.clear();
        interactive.process("(A andthen B) unless C");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a andthen b) unless c"});
        CHECK_THROWS_WITH_AS(interactive.process(R"(%(zelph/set-infix-display "policy" [["andthen" 20 :left]]))"),
                             doctest::Contains("unknown display scheme 'policy'"),
                             std::runtime_error);
    }
    SUBCASE("the next run-delta is a full pass")
    {
        // The session has run, thus its delta record is valid, and the session
        // and the file contain an identical count of rules. Performing seeding
        // from that record after the load skipped the classic pass, and the
        // file's `john ~ man` never met the rule.
        process_lines(interactive, "(X ~ man) => (X ~ mortal)\nbill ~ man");
        interactive.process(".load \"" + rules.string() + "\"");

        collector.clear();
        interactive.process(".run-delta");
        CHECK(any_event_contains(collector, "Incremental run not applicable"));
        collector.clear();
        interactive.process("X ~ mortal");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john ~ mortal"});
    }
    SUBCASE("zelph/load replaces as .load does")
    {
        session();
        interactive.process("%(zelph/load \"" + plain.generic_string() + "\")");

        collector.clear();
        interactive.process("X ~ man");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john ~ man"});
        CHECK_THROWS_WITH_AS(interactive.process(".node bill"), doctest::Contains("No node found with name 'bill'"), std::runtime_error);
    }

    fs::remove(plain);
    fs::remove(rules);
    fs::remove(self_fact);
    fs::remove(list);
    fs::remove(term);
}

// Annotations associated with a session are tied to its nodes, and a load
// replaces them. Node identifiers are positions: the session's `x`
// and the file's `john` take the same identifier, meaning an annotation
// referencing x that persisted past the load would be interpreted as
// belonging to john. The file's original annotations occupy their position
// instead; a partial load retrieves none, since a view contains no
// annotations.
TEST_CASE("annotations: a replacing load takes them along")
{
    const fs::path annotated = fs::temp_directory_path() / "zelph_load_annotations.bin";
    const fs::path bare      = fs::temp_directory_path() / "zelph_load_annotations_bare.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
john ~ man
%(zelph/annotate "john" "source" "from the file")
)");
        interactive.process(".save \"" + annotated.string() + "\"");
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("john ~ man");
        interactive.process(".save \"" + bare.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
x ~ man
%(def x (zelph/lookup "x"))
%(zelph/annotate x "source" "from the session")
)");

    SUBCASE("a file with annotations")
    {
        interactive.process(".load \"" + annotated.string() + "\"");
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "john")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == R"({"source" ("from the file")})");
    }
    SUBCASE("a file without annotations")
    {
        interactive.process(".load \"" + bare.string() + "\"");
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "john")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == "{}");
    }
    SUBCASE("a partial load")
    {
        interactive.process(".load-partial \"" + annotated.string() + "\"");
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "john")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == "{}");
    }
    SUBCASE("a file that is not a network")
    {
        // A file that does not constitute a network is rejected at the
        // header stage, before any interaction with the graph: the session
        // keeps its graph, along with the annotations attached to its nodes.
        const fs::path garbage = fs::temp_directory_path() / "zelph_load_annotations_garbage.bin";
        std::ofstream(garbage, std::ios::binary) << "hello, this is not a serialized network";
        CHECK_THROWS_AS(interactive.process(".load \"" + garbage.string() + "\""), std::runtime_error);
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "x")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == R"({"source" ("from the session")})");
        fs::remove(garbage);
    }

    fs::remove(annotated);
    fs::remove(bare);
}

TEST_CASE("bin inspection: a mistyped chunk selector leaves the session alone")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // The selectors are checked before the graph is touched. They used to be
    // checked after it had been discarded, which left a network without even
    // its core nodes: every following statement failed with "requested left
    // node 1 does not exist", and nothing said that .new was the way out.
    CHECK_THROWS_AS(interactive.process(".load-partial \"" + f.good.string() + "\" left=99"),
                    std::runtime_error);

    collector.clear();
    interactive.process("a p b");
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    fs::remove_all(f.root);
}

TEST_CASE("load-partial: a partial load marks the label index incomplete")
{
    // Every load retrieves the carriers associated with a shared label,
    // drawing from both name sections. When a load examines only a portion of
    // their chunks, it identifies only a subset of the carriers, and
    // responding based on that partial set would constitute a silent error;
    // the response indicates it cannot provide an answer.
    using namespace wikidata_dump;
    const auto     dump = write_lines("zelph_partial_labels",
                                      {entity("Q8441", text("en", "man")),
                                       entity("Q29719014", text("en", "man")),
                                       entity("Q709369", text("en", "man"))});
    const fs::path file = fs::temp_directory_path() / "zelph_partial_labels_saved.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    interactive.process(".save \"" + file.string() + "\"");
    const std::vector<std::string> carriers{"Q8441", "Q709369", "Q29719014"};

    SUBCASE("a selection of the name chunks")
    {
        interactive.process(".load-partial \"" + file.string() + "\" nameOfNode=0 nodeOfName=0");
        CHECK_FALSE(interactive.graph()->nodes_named("man", "en").has_value());

        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == carriers);
    }

    SUBCASE("a whole load after it")
    {
        // A whole load replaces the graph that the partial one left
        // behind, along with its associated mark: there is no need for a
        // .new in between.
        interactive.process(".load-partial \"" + file.string() + "\" nameOfNode=0 nodeOfName=0");
        CHECK_FALSE(interactive.graph()->nodes_named("man", "en").has_value());

        interactive.process(".load \"" + file.string() + "\"");
        CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == carriers);
    }

    SUBCASE("every name chunk")
    {
        interactive.process(".load-partial \"" + file.string() + "\" left=0");
        CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == carriers);
    }

    fs::remove(file);
    remove_dump(dump);
}
