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
#include "string/string_utils.hpp"
#include "test_helpers.hpp"
#include "wikidata_dump.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace zelph::test;
using namespace zelph::test::wikidata_dump;

namespace
{
    // One dump line per entity, structurally faithful to the real dump: the
    // English label carries JSON escapes, because the dump escapes every
    // non-ASCII character.
    std::string item(const std::string& qid, const std::string& escaped_label)
    {
        return "{\"type\":\"item\",\"id\":\"" + qid
             + "\",\"labels\":{\"en\":{\"language\":\"en\",\"value\":\"" + escaped_label
             + "\"}},\"descriptions\":{},\"aliases\":{},\"claims\":{\"P279\":[{\"mainsnak\":"
               "{\"snaktype\":\"value\",\"property\":\"P279\",\"datavalue\":{\"value\":"
               "{\"entity-type\":\"item\",\"numeric-id\":2,\"id\":\"Q2\"},\"type\":"
               "\"wikibase-entityid\"},\"datatype\":\"wikibase-item\"},\"type\":\"statement\","
               "\"id\":\""
             + qid + "$AAA\",\"rank\":\"normal\"}]},\"sitelinks\":{}}";
    }

    std::filesystem::path write_dump()
    {
        const auto path = std::filesystem::temp_directory_path() / "zelph_label_import_test.json";

        std::ofstream out(path, std::ios::binary);
        out << "[\n"
            << item("Q1", "B\\u00fcdner") << ",\n"                  // BMP escape
            << item("Q3", "The \\\"Chirping\\\" Crickets") << ",\n" // escaped quote
            << item("Q4", "clef \\ud834\\udd1e sign") << ",\n"      // surrogate pair
            << item("Q5", "back\\\\slash") << "\n"                  // escaped backslash
            << "]\n";
        return path;
    }
}

TEST_CASE("wikidata import: JSON escapes in labels are decoded" * doctest::test_suite("slow"))
{
    const auto dump = write_dump();

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        // Re-import rather than reading the cache the previous subcase wrote.
        std::filesystem::path cache = dump;
        cache.replace_extension(".bin");
        std::filesystem::remove(cache);

        interactive.process(".load \"" + dump.string() + "\"");
        interactive.process(".lang en");

        collector.clear();
        interactive.process(".clist 10");

        CHECK(any_event_contains(collector, "Büdner"));
        CHECK(any_event_contains(collector, "clef 𝄞 sign"));
        CHECK(any_event_contains(collector, "back\\slash"));
        // An escaped quote used to truncate the label to "The \".
        CHECK(any_event_contains(collector, "The \"Chirping\" Crickets"));
        // Nothing may reach the network as the escape sequence itself.
        CHECK_FALSE(any_event_contains(collector, "u00fc"));
        CHECK_FALSE(any_event_contains(collector, "ud834")); });

    remove_dump(dump);
}

TEST_CASE("wikidata import: a decoded label is addressable under its real name")
{
    const auto dump = write_dump();

    run_single_core_mode([&](auto& collector, auto& interactive)
                         {
        std::filesystem::path cache = dump;
        cache.replace_extension(".bin");
        std::filesystem::remove(cache);

        interactive.process(".load \"" + dump.string() + "\"");
        interactive.process(".lang en");

        collector.clear();
        interactive.process(".node Büdner");
        CHECK(any_event_contains(collector, "Q1"));

        collector.clear();
        CHECK_THROWS(interactive.process(".node B\\u00fcdner")); });

    remove_dump(dump);
}

// ---------------------------------------------------------------------------
// Labels held in common by several entities. Wikidata assigns a single label
// to numerous items: "man" refers to the adult male (Q8441), the Unix command
// displaying manual pages (Q709369), and a genetic component in the fruit fly
// (Q29719014), while 108 items bear the label "Berlin". The import gave the
// `en` name to the final item among those processed by its threads, causing
// every prior one to lose its English name: the intended item held its label
// for none of the 24 test words, and neither P31 nor P279 possessed an
// English name. Each carrier now preserves the label, and the name → node
// entry is directed to the lowest-ranked one (Zelph::label_precedes).
// ---------------------------------------------------------------------------

TEST_CASE("wikidata import: every item keeps its label, and the lowest number holds the name")
{
    // Q8441 appeared at the beginning of the file, while the other two
    // came afterwards: since the last line took precedence, the label
    // was assigned to Q709369 and Q8441 possessed no English name.
    const auto dump = write_lines("zelph_label_holder",
                                  {entity("Q8441", text("en", "man")),
                                   entity("Q1", text("en", "universe")),
                                   entity("Q29719014", text("en", "man")),
                                   entity("Q709369", text("en", "man"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    CHECK(holder_of(interactive, "man") == "Q8441");
    CHECK(en_name_of(interactive, "Q8441") == "man");
    CHECK(en_name_of(interactive, "Q29719014") == "man");
    CHECK(en_name_of(interactive, "Q709369") == "man");

    // The input a user enters arrives at the holder.
    interactive.process(".lang en");
    collector.clear();
    interactive.process(".node man");
    CHECK(any_event_contains(collector, "Q8441"));

    remove_dump(dump);
}

TEST_CASE("wikidata import: the holder does not depend on the line order")
{
    // Four threads take batches of lines, so the label that arrives last
    // depends on timing. The holder is the minimum among the carriers, and
    // a minimum operation is indifferent to the sequence in which it is
    // taken.
    const std::vector<std::string> lines{entity("Q8441", text("en", "man")),
                                         entity("Q29719014", text("en", "man")),
                                         entity("P17", text("en", "country")),
                                         entity("Q6256", text("en", "country")),
                                         entity("Q709369", text("en", "man")),
                                         entity("Q64", text("en", "Berlin")),
                                         entity("Q821244", text("en", "Berlin"))};

    // label → the identifier of its holder, and id → its label,
    // ensuring that two imports are comparable even when they assign
    // node numbers differently.
    using NameMaps       = std::pair<std::map<std::string, std::string>, std::map<std::string, std::string>>;
    const auto name_maps = [](const zelph::console::Interactive& interactive)
    {
        NameMaps maps;
        for (const auto& [node, label] : interactive.graph()->get_nodes_in_language("en"))
        {
            maps.second[interactive.graph()->get_name(node, "wikidata", false)] = std::string(label);
            maps.first[std::string(label)]                                      = holder_of(interactive, std::string(label));
        }
        return maps;
    };

    const auto import = [&](const std::vector<std::string>& ordered)
    {
        const auto                  dump = write_lines("zelph_label_order", ordered);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + dump.string() + "\"");
        NameMaps maps = name_maps(interactive);
        remove_dump(dump);
        return maps;
    };

    const NameMaps forward = import(lines);
    const NameMaps reverse = import(std::vector<std::string>(lines.rbegin(), lines.rend()));

    CHECK(forward == reverse);
    CHECK(forward.first.at("man") == "Q8441");
    CHECK(forward.first.at("Berlin") == "Q64");
    CHECK(forward.second.size() == lines.size());
}

TEST_CASE("wikidata import: a property holds a label before an item")
{
    // Properties and items have `en` names in common. "country" functions
    // as both P17 and Q6256, and after the item was handled later, the
    // property lost its name; P31 "instance of" and P279 "subclass of"
    // similarly surrendered their names to two items in the same way. A
    // label situated within a relation position means the property.
    const auto dump = write_lines("zelph_label_property",
                                  {entity("P17", text("en", "country")),
                                   entity("Q6256", text("en", "country"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    CHECK(holder_of(interactive, "country") == "P17");
    CHECK(en_name_of(interactive, "Q6256") == "country");

    remove_dump(dump);
}

TEST_CASE("wikidata import: an item without an English label takes its mul label")
{
    // Wikidata keeps a name shared across all languages using the code `mul`,
    // while Douglas Adams (Q42), Albert Einstein (Q937), and Marie Curie
    // (Q7186) lack any entry under `en`: a recent import from a current dump
    // omitted their English name. The `mul` label functions as a contender
    // against `en` labels from other items, just like any other carrier.
    const auto dump = write_lines("zelph_label_mul",
                                  {entity("Q5301194", text("en", "Douglas Adams")),
                                   entity("Q42", text("mul", "Douglas Adams")),
                                   entity("Q75824389", text("en", "Douglas Adams")),
                                   entity("Q937", text("mul", "Albert Einstein") + "," + text("en", "A. Einstein"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    CHECK(holder_of(interactive, "Douglas Adams") == "Q42");
    CHECK(en_name_of(interactive, "Q42") == "Douglas Adams");
    CHECK(en_name_of(interactive, "Q5301194") == "Douglas Adams");
    CHECK(en_name_of(interactive, "Q75824389") == "Douglas Adams");

    SUBCASE("an English label wins over mul")
    {
        // The `mul` entry is intentionally placed first within the labels
        // object: the order of the entries does not decide.
        CHECK(en_name_of(interactive, "Q937") == "A. Einstein");
        CHECK(holder_of(interactive, "Albert Einstein").empty());
    }

    remove_dump(dump);
}

TEST_CASE("wikidata import: a label is searched only inside the labels object")
{
    // The quest to locate the label ran from the id until the end of the
    // line. It is confined within the labels object, ensuring that a
    // description or an alias in the language, or beneath `mul`, never serves
    // as the name of an item lacking a label in that language.
    const auto dump = write_lines("zelph_label_bounded",
                                  {entity("Q100",
                                          text("de", "Ding"),
                                          "",
                                          text("en", "a description") + "," + text("mul", "a mul description"),
                                          "\"en\":[{\"language\":\"en\",\"value\":\"an alias\"}],"
                                          "\"mul\":[{\"language\":\"mul\",\"value\":\"a mul alias\"}]")});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    REQUIRE(entity_node(interactive, "Q100") != 0);
    CHECK(en_name_of(interactive, "Q100").empty());

    remove_dump(dump);
}

TEST_CASE("wikidata import: the import writes the marker, and .cleanup keeps it")
{
    // A network that has labels assigned in this manner indicates its origin
    // via a node named `wikidata:labels-v2` in the `zelph` language; a
    // resolver capable of mapping terms to items using their labels can
    // distinguish this network from one that was imported earlier. Nothing
    // refers to the node, and the recipe that cuts the published networks ends
    // with .cleanup, eliminating disconnected nodes.
    const auto dump = write_lines("zelph_label_marker", {entity("Q8441", text("en", "man"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    const auto has_marker = [&]
    { return interactive.graph()->get_node(zelph::network::Zelph::wikidata_labels_marker_name(), "zelph", true) != 0; };

    CHECK(has_marker());

    interactive.process(".cleanup");
    CHECK(has_marker());

    namespace fs         = std::filesystem;
    const fs::path saved = fs::temp_directory_path() / "zelph_label_marker_saved.bin";

    SUBCASE("through .save and .load")
    {
        interactive.process(".save \"" + saved.string() + "\"");
        interactive.process(".new");
        interactive.process(".load \"" + saved.string() + "\"");
        CHECK(has_marker());
    }

    SUBCASE("into a predicate slice")
    {
        // The slice keeps the labels of the classes it preserves, assigned
        // in the same way.
        interactive.process(".lang wikidata");
        interactive.process("Q8441 P279 Q5");
        interactive.process(".save-predicates \"" + saved.string() + "\" P279");
        interactive.process(".new");
        interactive.process(".load \"" + saved.string() + "\"");
        CHECK(has_marker());
    }

    fs::remove(saved);
    remove_dump(dump);
}

TEST_CASE("wikidata import: .name keeps its takeover semantics after an import")
{
    // The assignment by rank belongs exclusively to the importer. In all
    // other contexts, a name goes to its most recent recipient, regardless
    // of rank, and the prior holder relinquishes it -- that is the
    // guarantee provided by `.name` and Zelph::set_name.
    const auto dump = write_lines("zelph_label_takeover",
                                  {entity("Q8441", text("en", "man")),
                                   entity("Q29719014", text("en", "man")),
                                   entity("Q709369", text("en", "man"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    auto* const graph = interactive.graph();

    SUBCASE("set_name takes the label from a holder that ranks first")
    {
        interactive.process("mine p q");
        const zelph::network::Node mine = graph->get_node("mine", "zelph");
        REQUIRE(mine != 0);

        graph->set_name(mine, "man", "en", false);
        CHECK(graph->get_node("man", "en") == mine);
        CHECK(en_name_of(interactive, "Q8441").empty());
        // The remaining carriers keep their label; the takeover applies
        // solely to the holder.
        CHECK(en_name_of(interactive, "Q29719014") == "man");
        CHECK(en_name_of(interactive, "Q709369") == "man");
    }

    SUBCASE(".name merges a node with the holder of the label")
    {
        interactive.process("mine p q");
        interactive.process(".name mine en man");
        CHECK(any_event_contains(collector, "Merging them"));

        const zelph::network::Node merged = graph->get_node("man", "en");
        CHECK(merged == graph->get_node("mine", "zelph"));
        CHECK(graph->get_name(merged, "wikidata", false) == "Q8441");
        CHECK(en_name_of(interactive, "Q709369") == "man");
    }

    remove_dump(dump);
}

TEST_CASE("wikidata import: every carrier of a shared label is listed, in rank order")
{
    // A word that attains Wikidata via its label needs every item that
    // carries it, not merely the one the label resolves to: "man" corresponds
    // to three items in this context and eight across the complete graph,
    // with the selection among them falling to the caller's business. The
    // sequence follows the ranking by which the holder is selected, ensuring
    // consistency between two calls and making the initial response the
    // designated holder.
    const auto dump = write_lines("zelph_label_carriers",
                                  {entity("Q29719014", text("en", "man")),
                                   entity("Q8441", text("en", "man")),
                                   entity("Q709369", text("en", "man")),
                                   entity("Q6256", text("en", "country")),
                                   entity("P17", text("en", "country")),
                                   entity("Q1", text("en", "universe"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    auto* const graph = interactive.graph();

    const std::size_t nodes = node_count(collector, interactive);

    CHECK(ids_of(interactive, graph->nodes_named("man", "en")) == std::vector<std::string>{"Q8441", "Q709369", "Q29719014"});
    CHECK(ids_of(interactive, graph->nodes_named("country", "en")) == std::vector<std::string>{"P17", "Q6256"});
    CHECK(ids_of(interactive, graph->nodes_named("universe", "en")) == std::vector<std::string>{"Q1"});
    CHECK(ids_of(interactive, graph->nodes_named("nothing", "en")).empty());

    // Inquiring creates nothing.
    CHECK(node_count(collector, interactive) == nodes);

    remove_dump(dump);
}

TEST_CASE("wikidata import: the index after a .save/.load equals the one the import built")
{
    // The label's carriers are not part of a .bin section: a load reacquires
    // them through the node -> name entries, which hold the label for each
    // carrier. What a load finds must match what the import was aware of.
    const auto dump = write_lines("zelph_label_reload",
                                  {entity("Q8441", text("en", "man")),
                                   entity("Q29719014", text("en", "man")),
                                   entity("Q709369", text("en", "man")),
                                   entity("P17", text("en", "country")),
                                   entity("Q6256", text("en", "country")),
                                   entity("Q42", text("mul", "Douglas Adams")),
                                   entity("Q5301194", text("en", "Douglas Adams"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    const auto carriers = [&]
    {
        std::map<std::string, std::vector<std::string>> all;
        for (const std::string label : {"man", "country", "Douglas Adams"})
            all[label] = ids_of(interactive, interactive.graph()->nodes_named(label, "en"));
        return all;
    };

    const auto imported = carriers();
    CHECK(imported.at("man").size() == 3);
    CHECK(interactive.graph()->label_index_consistent());

    namespace fs         = std::filesystem;
    const fs::path saved = fs::temp_directory_path() / "zelph_label_reload_saved.bin";
    interactive.process(".save \"" + saved.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + saved.string() + "\"");

    CHECK(carriers() == imported);
    CHECK(interactive.graph()->label_index_consistent());

    fs::remove(saved);
    remove_dump(dump);
}

// A predicate slice keeps the names of the nodes it retains. If it preserves
// carriers linked to a label but omits the node that the label resolves to, the
// file records the kept carrier ranked first as that node -- the one selected
// by a prune operation on the dropped node. This supersedes the test "a slice
// without a label's holder hands the label to a carrier it kept", which pinned
// the same carrier, reached the other way round: the file stored carriers
// without a name-to-node mapping, and the load process chose one, resulting in
// a network being loaded that differed from the one originally written. A
// .load-partial that includes every name → node chunk and excludes any node →
// name chunk does not elect, so in this case the label resolves solely based on
// the file's recorded data. Q29719014 comes first in both the dump and the
// slice, and Q709369 is ranked first.
TEST_CASE("wikidata import: a slice that leaves a label's holder behind records the carrier ranked first")
{
    const auto dump = write_lines("zelph_label_slice",
                                  {entity("Q29719014", text("en", "man"), claim("P279", "Q5")),
                                   entity("Q709369", text("en", "man"), claim("P279", "Q5")),
                                   entity("Q8441", text("en", "man"), claim("P31", "Q5"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    REQUIRE(holder_of(interactive, "man") == "Q8441");
    const zelph::network::Node q709369 = entity_node(interactive, "Q709369");

    namespace fs         = std::filesystem;
    const fs::path slice = fs::temp_directory_path() / "zelph_label_slice_saved.bin";
    interactive.process(".lang wikidata");
    interactive.process(".save-predicates \"" + slice.string() + "\" P279");
    interactive.process(".new");

    SUBCASE("the file records it")
    {
        interactive.process(".load-partial \"" + slice.string() + "\" nameOfNode=none");
        CHECK(interactive.graph()->get_node("man", "en") == q709369);
    }
    SUBCASE("a load takes it from the file, without a note")
    {
        collector.clear();
        interactive.process(".load \"" + slice.string() + "\"");
        CHECK_FALSE(any_output_contains(collector, "carried by nodes"));
        CHECK(entity_node(interactive, "Q8441") == 0);
        CHECK(holder_of(interactive, "man") == "Q709369");
        CHECK(interactive.graph()->label_index_consistent());
    }

    fs::remove(slice);
    remove_dump(dump);
}

namespace zelph::test
{
    // Binary files from older_networks/*.bin are integrated directly into
    // this compiled executable via embed_files.cmake.
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

// The file `older_networks/slice_without_label_holders.bin` was written by an
// engine that dropped a label's holder from a slice without handing the label
// on, via `.save-predicates slice_without_label_holders.bin P279` (in language
// wikidata) following the import of five dump lines: Q29719014, Q709369 and
// Q8441 labelled "man", Q75824389 and Q42 labelled "Douglas Adams"; only Q8441
// and Q42 specify P31 rather than P279. The file contains the labels of the
// three retained carriers and lacks any name → node entry in `en`, the shape of
// a network pruned by an engine before re-election as well. A load gives each
// label to the carrier ranked first, an action not documented in the file,
// hence it says how many labels were assigned to a holder; a subsequent `.save`
// operation records these assignments, and the next load finds nothing to say.
TEST_CASE("wikidata import: a file without a label's holder loads with one, and the load says so")
{
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("slice_without_label_holders");
    REQUIRE(network != networks.end());

    namespace fs        = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "zelph_older_slice_without_label_holders.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + path.string() + "\"");
    CHECK(any_output_contains(collector, "Note: 2 labels in this file are carried by nodes but resolve to none of them"));
    CHECK(holder_of(interactive, "man") == "Q709369");
    CHECK(holder_of(interactive, "Douglas Adams") == "Q75824389");
    CHECK(interactive.graph()->label_index_consistent());

    interactive.process(".save \"" + path.string() + "\"");
    interactive.process(".new");
    collector.clear();
    interactive.process(".load \"" + path.string() + "\"");
    CHECK_FALSE(any_output_contains(collector, "carried by nodes"));
    CHECK(holder_of(interactive, "man") == "Q709369");
    CHECK(holder_of(interactive, "Douglas Adams") == "Q75824389");

    fs::remove(path);
}

// During the process of importing a dump, information is integrated into the
// session according to the Wikidata ID, a mechanism that test_check_fact.cpp
// depends upon. The cache generated during the import consists of the dump
// being loaded into an empty graph, and a later .load of the dump will use
// this cached version only if the import starts with an empty graph; in any
// other graph, the dump is imported and added, and the cache is neither read
// nor updated. Previously, this case pinned that an existing cache would
// override the session's graph regardless of its contents: whether a
// .load of the dump kept the session's facts was determined by the presence
// of a file adjacent to the dump, and the cache written by an import into a
// session contained the session's own facts, which every later .load of the
// dump would restore.
TEST_CASE("wikidata import: the cache stands in for the import into an empty graph only")
{
    namespace fs = std::filesystem;

    const auto dump  = write_dump();
    fs::path   cache = dump;
    cache.replace_extension(".bin");
    fs::remove(cache);

    const auto holds = [](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& lang, const std::string& query, const std::string& answer)
    {
        interactive.process(".lang " + lang);
        collector.clear();
        interactive.process(query);
        return answers_contain(collector, answer);
    };

    // The dump's cache as written, after which the dump changes: Q1 is
    // subsequently found exclusively in the cache, Q7 solely within the dump.
    const auto cache_then_change_dump = [&]
    {
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".load \"" + dump.string() + "\"");
        }
        REQUIRE(fs::exists(cache));
        std::ofstream(dump, std::ios::binary) << "[\n"
                                              << item("Q7", "seven") << "\n]\n";
    };

    const auto bytes = [](const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    SUBCASE("without a cache, the dump is added, and no cache is written")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("bill likes ann");
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(holds(collector, interactive, "wikidata", "Q1 P279 X", "Q1 P279 Q2"));
        CHECK(holds(collector, interactive, "zelph", "X likes Y", "bill likes ann"));
        CHECK_FALSE(fs::exists(cache));
    }
    SUBCASE("with a cache, an empty graph becomes the cache")
    {
        cache_then_change_dump();

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(holds(collector, interactive, "wikidata", "Q1 P279 X", "Q1 P279 Q2"));
        CHECK_FALSE(holds(collector, interactive, "wikidata", "Q7 P279 X", "Q7 P279 Q2"));
    }
    SUBCASE("a module that only defines Janet code leaves the graph empty")
    {
        // The SPARQL layer registers a keyword and defines functions, and
        // writes no node.
        cache_then_change_dump();

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".import sparql");
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(holds(collector, interactive, "wikidata", "Q1 P279 X", "Q1 P279 Q2"));
        CHECK_FALSE(holds(collector, interactive, "wikidata", "Q7 P279 X", "Q7 P279 Q2"));
    }
    SUBCASE("a query that names a node leaves the graph not empty")
    {
        // A query creates the nodes corresponding to its names, just as a
        // statement does, meaning the dump is imported instead of being
        // retrieved from its cache.
        cache_then_change_dump();

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("X likes Y");
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(holds(collector, interactive, "wikidata", "Q7 P279 X", "Q7 P279 Q2"));
        CHECK_FALSE(holds(collector, interactive, "wikidata", "Q1 P279 X", "Q1 P279 Q2"));
    }
    SUBCASE("with a cache, a graph that is not empty gains the dump, and the cache stays as it is")
    {
        cache_then_change_dump();
        const std::string cached = bytes(cache);

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("bill likes ann");
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(any_output_contains(collector, "the cache " + cache.string() + " was left as it is"));
        CHECK(holds(collector, interactive, "zelph", "X likes Y", "bill likes ann"));
        CHECK(holds(collector, interactive, "wikidata", "Q7 P279 X", "Q7 P279 Q2"));
        CHECK_FALSE(holds(collector, interactive, "wikidata", "Q1 P279 X", "Q1 P279 Q2"));
        CHECK(bytes(cache) == cached);
    }
    SUBCASE("the cache named itself replaces the graph whatever it holds")
    {
        // A .bin named by the user remains a network file regardless of the
        // dump located beside it.
        cache_then_change_dump();

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("bill likes ann");
        interactive.process(".load \"" + cache.string() + "\"");
        CHECK(holds(collector, interactive, "wikidata", "Q1 P279 X", "Q1 P279 Q2"));
        CHECK_FALSE(holds(collector, interactive, "zelph", "X likes Y", "bill likes ann"));
    }

    remove_dump(dump);
}

TEST_CASE("wikidata import: a cache that cannot be written is a failure of the session")
{
    // Loading a dump via `.load` generates a corresponding .bin file for the
    // network, which is frequently the primary purpose of the load operation: a
    // regeneration script or a Makefile rule depends on this file as its
    // output. If the file could not be written, zelph said
    // "Failed to save cache" yet still returned 0, causing a caller checking
    // the status to mistakenly treat a non-existent file as if it were present.
    // The import process completed successfully, allowing the session to
    // continue, but only the status records the failure.
    const auto dump = std::filesystem::temp_directory_path() / "zelph_unwritable_cache_test.json";
    std::ofstream(dump, std::ios::binary) << "[\n"
                                          << item("Q1", "B\\u00fcdner") << "\n]\n";

    // A directory designated for the cache: no file can be written there on any
    // system or by any user, which a read-only directory fails to ensure for
    // root.
    std::filesystem::path cache = dump;
    cache.replace_extension(".bin");
    std::filesystem::remove_all(cache);
    std::filesystem::create_directory(cache);

    run_single_core_mode([&](auto& collector, auto& interactive)
                         {
        CHECK_FALSE(interactive.had_failure());
        interactive.process(".load \"" + dump.string() + "\"");
        CHECK(any_event_contains(collector, "Failed to save cache"));
        CHECK(interactive.had_failure());

        interactive.process(".lang en");
        collector.clear();
        interactive.process(".node Büdner");
        CHECK(any_event_contains(collector, "Q1")); });

    std::filesystem::remove_all(cache);
    std::filesystem::remove(dump);
}

TEST_CASE("wikidata import: a cache that breaks off after its header is reported, and left as it is")
{
    // A cache prematurely terminated -- due to a full disk or an interrupted
    // copy -- encounters failure after the header, once the load has already
    // discarded the graph and begun reading part of the file. The dump is
    // subsequently imported into the incomplete state left by the cache,
    // resulting in a graph that is neither the cache nor the dump, a condition
    // that was reported solely as a diagnostic. The import further overwrote
    // the cache with this inconsistent graph, causing every later load of the
    // dump to read this corrupted mixture as the original dump. The session
    // documents the incident and outlines the steps to get a clean graph, its
    // status records the failure, and the cache, since this import did not
    // begin with an empty graph, remains unchanged. The interruption occurs
    // within the final chunk, far beyond the header.
    namespace fs = std::filesystem;

    const auto dump  = write_dump();
    fs::path   cache = dump;
    cache.replace_extension(".bin");
    fs::remove(cache);
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + dump.string() + "\"");
    }
    REQUIRE(fs::exists(cache));
    fs::resize_file(cache, fs::file_size(cache) - 8);

    const auto bytes = [](const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string cut = bytes(cache);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");

    CHECK(any_event_contains(collector, "WARNING: the cache " + cache.string() + " could not be read completely: Failed to read"));
    CHECK(any_event_contains(collector, "the dump is imported into it"));
    CHECK(any_event_contains(collector, ".new, delete " + cache.string() + ", and .load " + dump.string() + " again"));
    CHECK(interactive.had_failure());
    CHECK(bytes(cache) == cut);

    remove_dump(dump);
}

TEST_CASE("JSON unescaping covers the whole escape set")
{
    using zelph::string::unicode::unescape;

    CHECK(unescape("plain ASCII") == "plain ASCII");
    CHECK(unescape("gr\\u00fcn") == "grün");
    CHECK(unescape("\\u4e2d\\u6587") == "中文");

    // Code points above the BMP arrive as a surrogate pair and must become
    // one 4-byte sequence, not two 3-byte halves (which would be CESU-8).
    CHECK(unescape("\\ud834\\udd1e") == "𝄞");
    CHECK(unescape("\\ud834\\udd1e").size() == 4);

    // Unpaired surrogates cannot be represented; U+FFFD keeps the result
    // valid UTF-8 instead of emitting a lone surrogate.
    CHECK(unescape("\\ud834x") == "�x");
    CHECK(unescape("\\udd1e") == "�");
    CHECK(unescape("\\ud834\\u0041") == "�A");

    CHECK(unescape("a\\\"b") == "a\"b");
    CHECK(unescape("a\\\\b") == "a\\b");
    CHECK(unescape("a\\/b") == "a/b");
    CHECK(unescape("a\\tb") == "a\tb");
    CHECK(unescape("a\\nb") == "a\nb");
    CHECK(unescape("a\\rb") == "a\rb");
    CHECK(unescape("a\\bb") == "a\bb");
    CHECK(unescape("a\\fb") == "a\fb");

    // Not JSON escapes: kept verbatim rather than swallowed.
    CHECK(unescape("a\\qb") == "a\\qb");
    CHECK(unescape("\\u12") == "\\u12");
    CHECK(unescape("\\uzzzz") == "\\uzzzz");
    CHECK(unescape("trailing\\") == "trailing\\");
    CHECK(unescape("") == "");

    // A backslash escaping a backslash must not consume the next escape.
    CHECK(unescape("a\\\\\\u0041") == "a\\A");
    CHECK(unescape("a\\\\nb") == "a\\nb");
}

TEST_CASE("wikidata example: a multi-word name is one node, so the transitivity rule applies")
{
    // stdlib/examples/wikidata/wikidata.zph, cited in wikidata.md, stated
    // that `"has quality" ~ transitive relation`: without quotes, the name
    // consists of two objects, `transitive` and `relation`, and the rule
    // applying to `R ~ "transitive relation"` never saw "has quality". The
    // same applied to `"is for example" is inverse of "~"`, which asserted a
    // fact using the predicate `is`. The example does not require any
    // Wikidata data to be loaded.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import examples/wikidata/wikidata");

    interactive.process("a \"has quality\" b");
    interactive.process("b \"has quality\" c");
    collector.clear();
    interactive.process("a \"has quality\" X");
    CHECK(collect_answers(collector).size() == 2);
    CHECK(any_output_contains(collector, "has quality\" c"));

    collector.clear();
    interactive.process("\"is for example\" \"is inverse of\" X");
    CHECK(collect_answers(collector).size() == 1);
}
