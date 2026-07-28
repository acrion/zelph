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

#include "io/mermaid.hpp"
#include "io/output.hpp"
#include "network/zelph.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace zelph::network;

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }

    std::string read_file(const std::filesystem::path& p)
    {
        std::ifstream     in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// ---------------------------------------------------------------------------
// Neighbor budget at hub nodes.
//
// Ranking neighbors for the diagram uses their rendered representation, and
// rendering one costs a full recursive reconstruction. Ranking EVERY
// neighbor of a hub to display five made `.node` cost seconds: the
// canonical zero carries ~2000 neighbors once a symbolic workload is
// loaded, and the playground runs `.node` silently after every command.
// Candidates beyond a fixed bound are therefore pre-selected by node ID.
// The risk that introduces is a LYING placeholder -- the "N more" label
// must keep reporting the true total, not the pre-selected remainder.
// ---------------------------------------------------------------------------
TEST_CASE("mermaid: the hidden-neighbor count reports the true total at a hub")
{
    Zelph      z(null_handler());
    const Node hub = z.node("hub");
    const Node rel = z.node("rel");

    constexpr int hub_degree    = 500;
    constexpr int max_neighbors = 5;

    // Each fact draws hub -> fact, so the hub accumulates hub_degree
    // outgoing neighbors -- far past the ranking bound.
    for (int i = 0; i < hub_degree; ++i)
        z.fact(z.node("s" + std::to_string(i)), rel, {hub});

    const std::filesystem::path out = std::filesystem::temp_directory_path() / "zelph_test_mermaid_hub.html";
    std::filesystem::remove(out);

    zelph::io::gen_mermaid_html(&z, hub, out.string(), 1, max_neighbors, {}, false, false, true);

    REQUIRE(std::filesystem::exists(out));
    const std::string html = read_file(out);

    // hub_degree neighbors, max_neighbors of them drawn: the placeholder
    // must name the rest. A pre-selection that counted only its own
    // survivors would print 59 here instead of 495.
    CHECK(html.find("[... " + std::to_string(hub_degree - max_neighbors) + " more ...]") != std::string::npos);

    std::filesystem::remove(out);
}

// ---------------------------------------------------------------------------
// Label escaping.
//
// The diagram passes through two HTML layers: the page (mermaid reads the
// diagram back from a <div> with textContent, so the browser's HTML parser
// sees it first) and the label (mermaid inserts label text as HTML). A raw
// '<' is destructive at both. zelph list syntax puts one in every list
// label, which made the Jacobian verdict's diagram fail outright -- mermaid
// answered "Syntax error in text" and the playground showed nothing.
// Escaping BOTH levels makes them cancel: each layer decodes one step.
// ---------------------------------------------------------------------------
TEST_CASE("mermaid: labels survive both HTML layers of the generated page")
{
    Zelph z(null_handler());

    // Every character that either layer would otherwise eat or mis-parse.
    const std::string hostile = "x<y>z&\"q\"";
    const Node        subject = z.node(hostile);
    const Node        object  = z.node("plain");
    const Node        pred    = z.node("rel");
    z.fact(subject, pred, {object});

    const std::filesystem::path out = std::filesystem::temp_directory_path() / "zelph_test_mermaid_escape.html";
    std::filesystem::remove(out);

    zelph::io::gen_mermaid_html(&z, subject, out.string(), 1, 5, {}, false, false, true);

    REQUIRE(std::filesystem::exists(out));
    const std::string html = read_file(out);

    // Two levels of escaping: the page decodes one, mermaid the other, so
    // '<' reaches the rendered label intact.
    CHECK(html.find("&amp;lt;") != std::string::npos);
    CHECK(html.find("&amp;gt;") != std::string::npos);
    CHECK(html.find("&amp;quot;") != std::string::npos);

    // The raw form is what broke the diagram; it must not survive anywhere.
    CHECK(html.find(hostile) == std::string::npos);
    CHECK(html.find("x<y>z") == std::string::npos);

    // mermaid's own '#quot;'-style codes are not an option -- current
    // versions leak their internal placeholder into the rendered label.
    CHECK(html.find("#quot;") == std::string::npos);

    std::filesystem::remove(out);
}
