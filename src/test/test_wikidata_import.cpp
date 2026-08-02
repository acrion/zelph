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

#include "string/string_utils.hpp"
#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace zelph::test;

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
            << item("Q1", "B\\u00fcdner") << ",\n"                     // BMP escape
            << item("Q3", "The \\\"Chirping\\\" Crickets") << ",\n"    // escaped quote
            << item("Q4", "clef \\ud834\\udd1e sign") << ",\n"         // surrogate pair
            << item("Q5", "back\\\\slash") << "\n"                     // escaped backslash
            << "]\n";
        return path;
    }

    // A .json load writes a .bin cache next to the source; a second load
    // would read that cache instead of re-importing, so both go away.
    void remove_dump(const std::filesystem::path& dump)
    {
        std::filesystem::path cache = dump;
        cache.replace_extension(".bin");
        std::filesystem::remove(cache);
        std::filesystem::remove(dump);
    }
}

TEST_CASE("wikidata import: JSON escapes in labels are decoded")
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
