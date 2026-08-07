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
#include <sstream>
#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .wikidata-constraints <dump> <dir> writes one .zph per property entity,
// turning the P2302 constraint statements it understands into zelph rules.
// Three of the ~40 constraint types in the table carry a generator; the rest
// are exported as commented raw JSON, deliberately.
//
// The thing worth testing is not the text. It is that the text RUNS: the
// generated conditions used to be emitted without their parentheses, and
// "I P1 Y, I P31 Q5 => !" parses -- without any error -- as a set of two
// facts with the "=> !" swallowed into the second one. Every script the
// command had ever written defined zero rules and quietly added junk facts
// to whatever network imported it.
//
// The fixture is a property carrying one statement per case the generators
// can meet, plus an item (skipped) and a property without constraints.
// ---------------------------------------------------------------------------

namespace
{
    const char* kDump = R"json([
{"type":"property","datatype":"wikibase-item","id":"P9999","labels":{"en":{"language":"en","value":"probe property"}},"claims":{"P2302":[{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21502838,"id":"Q21502838"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2306":[{"snaktype":"value","property":"P2306","hash":"a1","datavalue":{"value":{"entity-type":"property","numeric-id":31,"id":"P31"},"type":"wikibase-entityid"},"datatype":"wikibase-property"}],"P2305":[{"snaktype":"value","property":"P2305","hash":"a2","datavalue":{"value":{"entity-type":"item","numeric-id":5,"id":"Q5"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P2306","P2305"],"id":"P9999$C1","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21502838,"id":"Q21502838"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2306":[{"snaktype":"value","property":"P2306","hash":"b1","datavalue":{"value":{"entity-type":"property","numeric-id":279,"id":"P279"},"type":"wikibase-entityid"},"datatype":"wikibase-property"}]},"qualifiers-order":["P2306"],"id":"P9999$C2","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21502838,"id":"Q21502838"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2305":[{"snaktype":"value","property":"P2305","hash":"c1","datavalue":{"value":{"entity-type":"item","numeric-id":5,"id":"Q5"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},{"snaktype":"value","property":"P2305","hash":"c2","datavalue":{"value":{"entity-type":"item","numeric-id":8,"id":"Q8"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}],"P2306":[{"snaktype":"value","property":"P2306","hash":"c3","datavalue":{"value":{"entity-type":"property","numeric-id":31,"id":"P31"},"type":"wikibase-entityid"},"datatype":"wikibase-property"}]},"qualifiers-order":["P2305","P2306"],"id":"P9999$C3","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":52558054,"id":"Q52558054"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2305":[{"snaktype":"value","property":"P2305","hash":"d1","datavalue":{"value":{"entity-type":"item","numeric-id":5,"id":"Q5"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},{"snaktype":"value","property":"P2305","hash":"d2","datavalue":{"value":{"entity-type":"item","numeric-id":8,"id":"Q8"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P2305"],"id":"P9999$C4","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":53869507,"id":"Q53869507"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P5314":[{"snaktype":"value","property":"P5314","hash":"e1","datavalue":{"value":{"entity-type":"item","numeric-id":54828448,"id":"Q54828448"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P5314"],"id":"P9999$C5","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21510860,"id":"Q21510860"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{},"qualifiers-order":[],"id":"P9999$C6","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":99999999,"id":"Q99999999"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{},"qualifiers-order":[],"id":"P9999$C7","rank":"normal"}]},"sitelinks":{}},
{"type":"property","datatype":"wikibase-item","id":"P7777","labels":{"en":{"language":"en","value":"requires probe"}},"claims":{"P2302":[{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21503247,"id":"Q21503247"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2306":[{"snaktype":"value","property":"P2306","hash":"f1","datavalue":{"value":{"entity-type":"property","numeric-id":279,"id":"P279"},"type":"wikibase-entityid"},"datatype":"wikibase-property"}]},"qualifiers-order":["P2306"],"id":"P7777$C1","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21503247,"id":"Q21503247"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2306":[{"snaktype":"value","property":"P2306","hash":"f2","datavalue":{"value":{"entity-type":"property","numeric-id":17,"id":"P17"},"type":"wikibase-entityid"},"datatype":"wikibase-property"}],"P2305":[{"snaktype":"value","property":"P2305","hash":"f3","datavalue":{"value":{"entity-type":"item","numeric-id":30,"id":"Q30"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},{"snaktype":"value","property":"P2305","hash":"f4","datavalue":{"value":{"entity-type":"item","numeric-id":183,"id":"Q183"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P2306","P2305"],"id":"P7777$C2","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21510864,"id":"Q21510864"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2306":[{"snaktype":"value","property":"P2306","hash":"f5","datavalue":{"value":{"entity-type":"property","numeric-id":569,"id":"P569"},"type":"wikibase-entityid"},"datatype":"wikibase-property"}]},"qualifiers-order":["P2306"],"id":"P7777$C3","rank":"normal"}]},"sitelinks":{}},
{"type":"item","id":"Q42","labels":{"en":{"language":"en","value":"an item"}},"claims":{},"sitelinks":{}},
{"type":"property","datatype":"string","id":"P8888","labels":{"en":{"language":"en","value":"no constraints"}},"claims":{},"sitelinks":{}}
]
)json";

    // A property whose qualifiers carry no entity value: a novalue P2306
    // next to a P2305 that does have one, and a none-of whose forbidden
    // value is a string.
    const char* kEdgeDump = R"json([
{"type":"property","datatype":"wikibase-item","id":"P7777","labels":{"en":{"language":"en","value":"edge cases"}},"claims":{"P2302":[{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":21502838,"id":"Q21502838"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2306":[{"snaktype":"novalue","property":"P2306","hash":"n1","datatype":"wikibase-property"}],"P2305":[{"snaktype":"value","property":"P2305","hash":"n2","datavalue":{"value":{"entity-type":"item","numeric-id":5,"id":"Q5"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P2306","P2305"],"id":"P7777$N1","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P2302","datavalue":{"value":{"entity-type":"item","numeric-id":52558054,"id":"Q52558054"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2305":[{"snaktype":"value","property":"P2305","hash":"s1","datavalue":{"value":"forbidden","type":"string"},"datatype":"string"}]},"qualifiers-order":["P2305"],"id":"P7777$N2","rank":"normal"}]},"sitelinks":{}}
]
)json";

    std::filesystem::path write_dump(const char* content, const char* name)
    {
        const auto    path = std::filesystem::temp_directory_path() / name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream     in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
}

TEST_CASE("wikidata constraints: the exported script defines rules that fire")
{
    namespace fs = std::filesystem;

    const auto dump = write_dump(kDump, "zelph_constraints_test.json");
    const auto dir  = fs::temp_directory_path() / "zelph_constraints_out";
    fs::remove_all(dir);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".wikidata-constraints \"" + dump.string() + "\" \"" + dir.string() + "\"");

    const auto script = dir / "P9999.zph";
    REQUIRE(fs::exists(script));

    const std::string text = read_file(script);

    // conflicts-with, one forbidden value; and the same constraint with the
    // qualifiers in the opposite order, which must read the same way.
    CHECK(text.find("(I P9999 Y, I P31 Q5) => !") != std::string::npos);
    CHECK(text.find("(I P9999 Y, I P31 Q8) => !") != std::string::npos);
    // conflicts-with without a value: the conflicting property alone.
    CHECK(text.find("(I P9999 Y, I P279 Z) => !") != std::string::npos);
    // none-of: one rule per forbidden value, no second condition.
    CHECK(text.find("(I P9999 Q5) => !") != std::string::npos);
    CHECK(text.find("(I P9999 Q8) => !") != std::string::npos);

    // A type in the table without a generator, and one that is not in the
    // table at all, both survive as comments rather than as broken zelph.
    CHECK(text.find("# Constraint: Q21510860") != std::string::npos);
    CHECK(text.find("# (no existing zelph rule generator") != std::string::npos);
    CHECK(text.find("# Unsupported constraint: Q99999999") != std::string::npos);

    // Item entities are not properties, and a property whose constraints
    // yield no rule is not a work-list entry: neither gets a script.
    CHECK_FALSE(fs::exists(dir / "Q42.zph"));
    CHECK_FALSE(fs::exists(dir / "P8888.zph"));

    // Now the part that was silently broken: import what was written.
    interactive.process(".deductions off");
    collector.clear();
    interactive.process(".import \"" + script.string() + "\"");
    interactive.process(".list-rules");
    CHECK_FALSE(any_output_contains(collector, "No rules found"));
    CHECK(any_output_contains(collector, "(I P9999 Q5) => !"));

    // ...and it decides something. P9999 together with P31 Q5 is forbidden.
    collector.clear();
    interactive.process(".lang wikidata");
    interactive.process("Q42 P9999 Q7");
    CHECK_FALSE(has_contradiction(collector));

    collector.clear();
    interactive.process("Q42 P31 Q5");
    CHECK(has_contradiction(collector));

    fs::remove_all(dir);
    fs::remove(dump);
}

TEST_CASE("wikidata constraints: a qualifier without a value produces no rule")
{
    namespace fs = std::filesystem;

    // The id search used to run past the end of the snak it started in, so a
    // qualifier that has no entity value of its own adopted the one next to
    // it. The conflicts-with statement below has a novalue P2306 and a P2305
    // pointing at Q5, and produced "(I P7777 Y, I Q5 Q5) => !" -- a rule that
    // would have flagged every item using the property together with a
    // property that does not exist.
    const auto dump = write_dump(kEdgeDump, "zelph_constraints_edge_test.json");
    const auto dir  = fs::temp_directory_path() / "zelph_constraints_edge_out";
    fs::remove_all(dir);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".wikidata-constraints \"" + dump.string() + "\" \"" + dir.string() + "\"");

    // Neither statement yields a rule, so the property does not appear in
    // the output at all. That absence is the assertion: before the fix the
    // file existed and held "(I P7777 Y, I Q5 Q5) => !".
    CHECK_FALSE(fs::exists(dir / "P7777.zph"));
    CHECK(fs::is_empty(dir));

    fs::remove_all(dir);
    fs::remove(dump);
}

TEST_CASE("wikidata constraints: the output directory is created, or refused with a message")
{
    namespace fs = std::filesystem;

    // The export creates the directory per entity on a worker thread, where
    // a filesystem_error is not a message but a std::terminate. Only the
    // last path component was created, so naming a target below a directory
    // that does not exist yet aborted the process as soon as the first
    // property entity arrived -- this test case used to take the whole test
    // binary down with it.
    const auto dump = write_dump(kDump, "zelph_constraints_dir_test.json");
    const auto root = fs::temp_directory_path() / "zelph_constraints_dir_root";
    fs::remove_all(root);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto nested = root / "a" / "b";
    interactive.process(".wikidata-constraints \"" + dump.string() + "\" \"" + nested.string() + "\"");
    CHECK(fs::exists(nested / "P9999.zph"));

    // A target that cannot be a directory -- below a regular file -- is a
    // reported error, not a crash and not a silent nothing.
    const auto blocker = fs::temp_directory_path() / "zelph_constraints_blocker";
    {
        std::ofstream(blocker) << "not a directory";
    }

    CHECK_THROWS_AS(
        interactive.process(".wikidata-constraints \"" + dump.string() + "\" \"" + (blocker / "out").string() + "\""),
        std::runtime_error);

    fs::remove(blocker);
    fs::remove_all(root);
    fs::remove(dump);
}

TEST_CASE("wikidata constraints: a required statement becomes a negated condition")
{
    // item-requires-statement (Q21503247) and value-requires-statement
    // (Q21510864) are the same sentence about the two ends of a statement:
    // an item using this property must ALSO carry a given one. They are the
    // second and eighth most frequent constraint types without a generator on
    // a real dump, and neither needs the class hierarchy -- which is what
    // makes them the ones to do first.
    namespace fs = std::filesystem;

    const auto dump = write_dump(kDump, "zelph_requires_test.json");
    const auto dir  = fs::temp_directory_path() / "zelph_requires_out";
    fs::remove_all(dir);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".wikidata-constraints \"" + dump.string() + "\" \"" + dir.string() + "\"");

    const auto script = dir / "P7777.zph";
    REQUIRE(fs::exists(script));
    const std::string text = read_file(script);

    // No allowed values: the required property alone.
    CHECK(text.find("(I P7777 Y, ¬(I P279 Z)) => !") != std::string::npos);
    // Allowed values: ONE rule with a negated condition per value, because
    // the violation is "none of them" -- a rule per value would report an
    // item that carries the first for not carrying the second.
    CHECK(text.find("(I P7777 Y, ¬(I P17 Q30), ¬(I P17 Q183)) => !") != std::string::npos);
    // The value end of the same sentence binds the VALUE instead.
    CHECK(text.find("(I P7777 Y, ¬(Y P569 Z)) => !") != std::string::npos);

    // And it decides something. The data goes in first and the rules after:
    // forward chaining is monotonic, so a NAF condition asked before the
    // fact that satisfies it exists would report a violation nothing can
    // retract.
    interactive.process(".deductions off");
    interactive.process(".lang wikidata");
    interactive.process(".auto-run");
    interactive.process("Q1 P7777 Q9");
    interactive.process("Q2 P7777 Q9");
    interactive.process("Q2 P279 Q10");
    interactive.process("Q2 P17 Q183");
    interactive.process("Q9 P569 Q11");
    interactive.process(".import \"" + script.string() + "\"");

    collector.clear();
    interactive.run(true, false, false);

    // Q1 satisfies none of the three, Q2 satisfies all three.
    CHECK(has_contradiction(collector));
    CHECK(any_output_contains(collector, "Q1 P7777 Q9"));
    CHECK_FALSE(any_output_contains(collector, "Q2 P7777 Q9"));

    fs::remove_all(dir);
    fs::remove(dump);
}
