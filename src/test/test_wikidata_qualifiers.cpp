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
#include <fstream>
#include <string>

using namespace zelph::test;

namespace
{
    // Minimal but structurally faithful Wikidata dump: one JSON entity per
    // line, wrapped in the dump's array brackets, entities terminated by a
    // trailing comma - exactly what ReadAsync delivers line by line.
    //
    // Q100: one P2738 statement (rank normal, main value Q900) with two
    //       P11260 item qualifiers and one P580 time qualifier.
    // Q200: one P39 statement (rank deprecated) with a P580 time qualifier,
    //       a P1111 quantity qualifier and a P582 novalue qualifier, plus
    //       one P31 statement without any qualifiers.
    const char* kDump = R"json([
{"type":"item","id":"Q100","labels":{"en":{"language":"en","value":"test union class"}},"claims":{"P2738":[{"mainsnak":{"snaktype":"value","property":"P2738","datavalue":{"value":{"entity-type":"item","numeric-id":900,"id":"Q900"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P11260":[{"snaktype":"value","property":"P11260","hash":"h1","datavalue":{"value":{"entity-type":"item","numeric-id":101,"id":"Q101"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},{"snaktype":"value","property":"P11260","hash":"h2","datavalue":{"value":{"entity-type":"item","numeric-id":102,"id":"Q102"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}],"P580":[{"snaktype":"value","property":"P580","hash":"h3","datavalue":{"value":{"time":"+2020-01-01T00:00:00Z","timezone":0,"before":0,"after":0,"precision":11,"calendarmodel":"http://www.wikidata.org/entity/Q1985727"},"type":"time"},"datatype":"time"}]},"qualifiers-order":["P11260","P580"],"id":"Q100$AAA1-1","rank":"normal"}]},"sitelinks":{}},
{"type":"item","id":"Q200","labels":{"en":{"language":"en","value":"test office"}},"claims":{"P39":[{"mainsnak":{"snaktype":"value","property":"P39","datavalue":{"value":{"entity-type":"item","numeric-id":30185,"id":"Q30185"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P580":[{"snaktype":"value","property":"P580","hash":"h4","datavalue":{"value":{"time":"+1999-05-01T00:00:00Z","timezone":0,"before":0,"after":0,"precision":11,"calendarmodel":"http://www.wikidata.org/entity/Q1985727"},"type":"time"},"datatype":"time"}],"P1111":[{"snaktype":"value","property":"P1111","hash":"h5","datavalue":{"value":{"amount":"+42","unit":"1"},"type":"quantity"},"datatype":"quantity"}],"P582":[{"snaktype":"novalue","property":"P582","hash":"h6","datatype":"time"}]},"qualifiers-order":["P580","P1111","P582"],"id":"Q200$BBB2-2","rank":"deprecated"}],"P31":[{"mainsnak":{"snaktype":"value","property":"P31","datavalue":{"value":{"entity-type":"item","numeric-id":5,"id":"Q5"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","id":"Q200$CCC3-3","rank":"normal"}]},"sitelinks":{}}
]
)json";

    std::filesystem::path write_dump()
    {
        const auto    path = std::filesystem::temp_directory_path() / "zelph_qualifier_import_test.json";
        std::ofstream out(path, std::ios::binary);
        out << kDump;
        return path;
    }

    void run_sparql(const zelph::console::Interactive& interactive, const std::string& query)
    {
        interactive.process("sparql");
        std::istringstream iss(query);
        std::string        line;
        while (std::getline(iss, line))
        {
            interactive.process(line);
        }
        interactive.process("");
    }
}

TEST_CASE("wikidata qualifiers: full import materializes statement structures")
{
    const auto dump = write_dump();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".wikidata-qualifiers \"" + dump.string() + "\"");
        interactive.process(".lang wikidata");

        collector.clear();
        interactive.process("Q100 p:P2738 _s");
        CHECK(answers_contain(collector, "Q100 p:P2738 Q100$AAA1-1"));

        collector.clear();
        interactive.process("Q100$AAA1-1 pq:P11260 _x");
        CHECK(answers_contain(collector, "Q100$AAA1-1 pq:P11260 Q101"));
        CHECK(answers_contain(collector, "Q100$AAA1-1 pq:P11260 Q102"));

        collector.clear();
        interactive.process("Q100$AAA1-1 ps:P2738 _v");
        CHECK(answers_contain(collector, "Q100$AAA1-1 ps:P2738 Q900"));

        // Scalar qualifier values keep their raw dump representation.
        collector.clear();
        interactive.process("Q200$BBB2-2 pq:P580 _t");
        CHECK(answers_contain(collector, "Q200$BBB2-2 pq:P580 +1999-05-01T00:00:00Z"));

        collector.clear();
        interactive.process("Q200$BBB2-2 pq:P1111 _q");
        CHECK(answers_contain(collector, "Q200$BBB2-2 pq:P1111 +42"));

        collector.clear();
        interactive.process("Q200$BBB2-2 wikibase:rank _r");
        CHECK(answers_contain(collector, "Q200$BBB2-2 wikibase:rank wikibase:DeprecatedRank"));

        // A novalue qualifier must not materialize a fact.
        collector.clear();
        interactive.process("Q200$BBB2-2 pq:P582 _n");
        CHECK(collect_answers(collector).empty());

        // A statement without qualifiers must not be materialized at all.
        collector.clear();
        interactive.process("Q200 p:P31 _s2");
        CHECK(collect_answers(collector).empty()); });
    std::filesystem::remove(dump);
}

TEST_CASE("wikidata qualifiers: selective import filters by qualifier property")
{
    const auto dump = write_dump();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".wikidata-qualifiers \"" + dump.string() + "\" P11260");
        interactive.process(".lang wikidata");

        collector.clear();
        interactive.process("Q100 p:P2738 _s");
        CHECK(answers_contain(collector, "Q100 p:P2738 Q100$AAA1-1"));

        // The P580 qualifier on Q100's statement is filtered out.
        collector.clear();
        interactive.process("Q100$AAA1-1 pq:P580 _t");
        CHECK(collect_answers(collector).empty());

        // Q200's P39 statement has no P11260 qualifier -> not materialized.
        collector.clear();
        interactive.process("Q200 p:P39 _s2");
        CHECK(collect_answers(collector).empty()); });
    std::filesystem::remove(dump);
}

TEST_CASE("wikidata qualifiers: paper disjointness query runs on imported qualifier data")
{
    const auto dump = write_dump();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process_file("sparql", {});
        interactive.process(".wikidata-qualifiers \"" + dump.string() + "\"");

        // The violation itself: Q300 is a subclass of both listed classes.
        process_lines(interactive, R"(
.lang wikidata
Q300 P279 Q101
Q300 P279 Q102
)");
        collector.clear();

        run_sparql(interactive, R"(SELECT DISTINCT ?i ?class ?disj1 ?disj2 WHERE {
  ?class p:P2738 ?l .
  MINUS { ?l wikibase:rank wikibase:DeprecatedRank . }
  ?l pq:P11260 ?disj1 . ?l pq:P11260 ?disj2 .
  FILTER ( ( str(?disj1) < str(?disj2) ) )
  ?i wdt:P279* ?disj1 . ?i wdt:P279* ?disj2 .
})");

        CHECK(any_output_contains(collector, "Q300 Q100 Q101 Q102"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
    std::filesystem::remove(dump);
}