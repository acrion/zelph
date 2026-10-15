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

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The persisted predicate index (<file>.bin.pidx.<predicate>).
//
// A transitive closure switches from the direct traversal to a per-predicate
// index once its scan budget is exhausted, and the index is written next to
// the .bin so later sessions skip the extraction. That sidecar is the only
// piece of engine state that OUTLIVES the process, so a change in how the
// extraction reads a relation makes every file written by an older binary
// wrong -- and wrong silently, because the closure then answers from the
// cache. The header therefore carries a format version.
//
// The fixture needs a graph large enough to blow the closure's scan budget
// (2^16 relation entries on the start node), so it is one hub with 66000
// facts pointing at it -- the smallest shape that reaches the index at all.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    constexpr size_t kFacts = 66000; // > 2^16 entries on the hub node

    struct Fixture
    {
        fs::path root;
        fs::path script;
        fs::path bin;
    };

    // The setup contained within this file, along with `extra` positioned
    // after the hub facts, resides in its own directory to ensure that no
    // two fixtures ever utilize the same sidecar.
    Fixture build_fixture(const char* directory = "zelph_pidx_test", const char* extra = "")
    {
        Fixture f;
        f.root   = fs::temp_directory_path() / directory;
        f.script = f.root / "hub.zph";
        f.bin    = f.root / "hub.bin";

        fs::remove_all(f.root);
        fs::create_directories(f.root);

        {
            std::ofstream out(f.script);
            out << ".deductions off\n.auto-run\n"; // .auto-run is a toggle: off
            for (size_t i = 0; i < kFacts; ++i)
            {
                out << "a" << i << " p hub\n";
            }
            out << extra;
        }

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".import \"" + f.script.string() + "\"");
        interactive.process(".save \"" + f.bin.string() + "\"");

        REQUIRE(fs::exists(f.bin));
        return f;
    }

    // The sidecar of predicate `p`; its node id is not known to the test, so
    // the one file matching the prefix is taken.
    fs::path sidecar_of(const Fixture& f)
    {
        for (const auto& entry : fs::directory_iterator(f.root))
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind("hub.bin.pidx.", 0) == 0) return entry.path();
        }
        return {};
    }

    // Load the fixture and run the closure that reaches the index.
    void load_and_close(const Fixture& f, zelph::console::Interactive& interactive)
    {
        interactive.process(".load \"" + f.bin.string() + "\"");
        interactive.process(R"(%(string "CLOSURE-" (length (zelph/closure-sources "hub" "p"))))");
    }

    void patch_version(const fs::path& sidecar, const uint32_t version)
    {
        std::fstream file(sidecar, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(file.good());
        file.seekp(4, std::ios::beg); // right after the "ZPIX" magic
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    }

    // A dedicated directory storing a copy of the fixture's network, ensuring
    // every test case begins without a sidecar and that the 66000 facts are
    // imported just once per process. The network declares a second
    // predicate, `likes`, such that a case concerning a fact of a different
    // predicate does not simultaneously constitute a case about a
    // declaration: the first fact of a NEW predicate declares it as a
    // relation type, which possesses a case of its own.
    Fixture fixture_copy(const char* directory)
    {
        static const Fixture shared = build_fixture("zelph_pidx_shared", "e0 likes e1\n");

        Fixture f;
        f.root = fs::temp_directory_path() / directory;
        f.bin  = f.root / "hub.bin";
        fs::remove_all(f.root);
        fs::create_directories(f.root);
        fs::copy_file(shared.bin, f.bin);
        return f;
    }

    // The count of closures so far that have been resolved through
    // constructing the index.
    std::size_t index_builds(const zelph::io::OutputCollector& collector)
    {
        std::size_t n = 0;
        for (const auto& event : collector.events())
            if (event.text.find("Building adjacency index") != std::string::npos) ++n;
        return n;
    }

    // The closure that reaches the index, and whether it answers `expected`
    // sources: read from the line it prints, not from any prior line.
    bool closure_counts(zelph::console::Interactive& interactive, zelph::io::OutputCollector& collector, const size_t expected)
    {
        interactive.process(R"(%(string "CLOSURE-" (length (zelph/closure-sources "hub" "p")) "."))");
        return last_out_text(collector).find("CLOSURE-" + std::to_string(expected) + ".") != std::string::npos;
    }
}

TEST_CASE("predicate index: a sidecar written by an older format is rebuilt, not trusted")
{
    const Fixture f = build_fixture();

    // First closure: builds the index and writes the sidecar.
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        load_and_close(f, interactive);
        CHECK(any_event_contains(collector, "Building adjacency index"));
        CHECK(any_event_contains(collector, "Saved adjacency index"));
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    }

    const fs::path sidecar = sidecar_of(f);
    REQUIRE(!sidecar.empty());

    // Second closure: the sidecar answers, no extraction happens.
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        load_and_close(f, interactive);
        CHECK(any_event_contains(collector, "Loaded adjacency index"));
        CHECK_FALSE(any_event_contains(collector, "Building adjacency index"));
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    }

    // A sidecar of the PREVIOUS format holds edges read off the adjacency:
    // one per fact that has an indexed fact as its subject, and none for a
    // self-fact. It must not be believed, however well its node counts match.
    patch_version(sidecar, 1);
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        load_and_close(f, interactive);
        CHECK_FALSE(any_event_contains(collector, "Loaded adjacency index"));
        CHECK(any_event_contains(collector, "Building adjacency index"));
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    }

    fs::remove_all(f.root);
}

// A relation-type declaration `p ~ ->` constitutes a fact of `~`, with p
// serving as the subject, thus occupying the same position in p's adjacency
// as any other fact of p, and the extraction read it as the pair (p, ->):
// after a closure established the index, `(zelph/targets "p" "p")` answered
// `->`, and each sidecar held one additional pair beyond the number of facts
// the predicate possesses. Since a sidecar in the previous format already
// holds that pair, it undergoes reconstruction rather than being read.
TEST_CASE("predicate index: a predicate's own declaration is no pair of its index" * doctest::test_suite("slow"))
{
    const Fixture f       = fixture_copy("zelph_pidx_own_declaration");
    const auto    targets = [](zelph::console::Interactive& interactive, zelph::io::OutputCollector& collector)
    {
        interactive.process(R"(%(string "TARGETS-[" (string/join (map zelph/name (zelph/targets "p" "p")) " ") "]"))");
        return last_out_text(collector).find("TARGETS-[]") != std::string::npos;
    };
    const std::string pairs = "(" + std::to_string(kFacts) + " edges)";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + f.bin.string() + "\"");
        CHECK(targets(interactive, collector));
        CHECK(closure_counts(interactive, collector, kFacts));
        CHECK(any_event_contains(collector, "Saved adjacency index"));
        CHECK(any_event_contains(collector, pairs));
        CHECK(targets(interactive, collector));
    }

    const fs::path sidecar = sidecar_of(f);
    REQUIRE(!sidecar.empty());

    bool rebuilt = false;
    SUBCASE("the next session reads the sidecar") {}
    SUBCASE("a sidecar of version 2 holds the pair and is rebuilt")
    {
        patch_version(sidecar, 2);
        rebuilt = true;
    }
    SUBCASE("a sidecar of a version this engine does not know is rebuilt")
    {
        // A format written by a more recent engine is just as unreadable to
        // this one as an older format, and is rebuilt in the same way.
        patch_version(sidecar, 0xffffffffu);
        rebuilt = true;
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    load_and_close(f, interactive);
    CHECK(any_event_contains(collector, "Loaded adjacency index") != rebuilt);
    CHECK(any_event_contains(collector, "Building adjacency index") == rebuilt);
    CHECK(any_event_contains(collector, pairs));
    CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    CHECK(targets(interactive, collector));

    fs::remove_all(f.root);
}

// A fact whose subject is the predicate, `p p hub`, looks in p's adjacency
// as the declaration `p ~ ->` does: a single bidirectional node, the
// predicate itself. Only the triple hash tells the two apart, hence the
// extraction pays for it in every such relation; skipping such relations
// instead would result in losing the fact's pair (p, hub) alongside the
// declaration. The pair remains present, within the session that builds the
// sidecar and within the session that loads it, while the declaration's
// pair (p, ->) is excluded from both.
TEST_CASE("predicate index: a fact with the predicate as its subject stays a pair of the index" * doctest::test_suite("slow"))
{
    const Fixture     f     = build_fixture("zelph_pidx_self_predicate", "p p hub\n");
    const std::string pairs = "(" + std::to_string(kFacts + 1) + " edges)";

    for (const char* sidecar : {"Building adjacency index", "Loaded adjacency index"})
    {
        CAPTURE(sidecar);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        const auto                  targets = [&]
        {
            interactive.process(R"(%(string "TARGETS-[" (string/join (map zelph/name (zelph/targets "p" "p")) " ") "]"))");
            return last_out_text(collector).find("TARGETS-[hub]") != std::string::npos;
        };

        interactive.process(".load \"" + f.bin.string() + "\"");
        CHECK(targets());
        CHECK(closure_counts(interactive, collector, kFacts + 1));
        CHECK(any_event_contains(collector, sidecar));
        CHECK(any_event_contains(collector, pairs));
        CHECK(targets());
    }

    fs::remove_all(f.root);
}

// A write invalidated the index of EVERY predicate and turned off the sidecars
// for the rest of the session: one unrelated fact following a .load, and the
// next class question on Wikidata rebuilt the P279 index over a million
// relation nodes, and wrote no sidecar for the next session. A fact involving
// a different predicate changes neither of them.
TEST_CASE("predicate index: a fact of another predicate keeps the built index and the sidecar" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_other_fact");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    interactive.process("x likes y");
    CHECK(closure_counts(interactive, collector, kFacts));
    CHECK(any_event_contains(collector, "Saved adjacency index"));

    interactive.process("w likes v");
    CHECK(closure_counts(interactive, collector, kFacts));
    CHECK(index_builds(collector) == 1);

    fs::remove_all(f.root);
}

// The initial fact associated with a predicate establishes its role as a
// relation type (`knows ~ ->`), and this declaration invalidated all indexes
// and deactivated sidecars for the current session: following a .load, each
// block introducing a new predicate triggered the next class question to
// regenerate the P279 index, and no sidecar was saved for the next session. The
// extraction reads adjacency exclusively, so a declaration affects only the
// index of `~` and the index of the predicate it declares. The sidecar written
// after the declaration is the one the next session will read, and the network
// saved with the new predicate answers both predicates.
TEST_CASE("predicate index: the first fact of a new predicate keeps the built index and the sidecar" * doctest::test_suite("slow"))
{
    const Fixture  f     = fixture_copy("zelph_pidx_new_predicate");
    const fs::path saved = f.root / "knows.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + f.bin.string() + "\"");

        interactive.process("x knows y");
        CHECK(closure_counts(interactive, collector, kFacts));
        CHECK(any_event_contains(collector, "Saved adjacency index"));

        interactive.process("w admires v");
        CHECK(closure_counts(interactive, collector, kFacts));
        CHECK(index_builds(collector) == 1);
        interactive.process(".save \"" + saved.string() + "\"");
    }

    SUBCASE("the next session reads the sidecar written after the declaration")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + f.bin.string() + "\"");
        CHECK(closure_counts(interactive, collector, kFacts));
        CHECK(any_event_contains(collector, "Loaded adjacency index"));
        CHECK(index_builds(collector) == 0);
    }
    SUBCASE("the saved network answers the closure and the new predicate")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + saved.string() + "\"");
        CHECK(closure_counts(interactive, collector, kFacts));
        collector.clear();
        interactive.process("S knows O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x knows y"});
    }

    fs::remove_all(f.root);
}

// The complementary aspect of narrowing: a declaration constitutes a fact
// of `~` (`knows ~ ->`), thereby rendering the index of `~` stale. A class
// question that passes a class containing numerous facts reads that index,
// and here `knows` functions as a class along the path from a0 before it
// becomes a predicate; upon declaration, its own class `->` also becomes
// part of the path.
TEST_CASE("predicate index: a declaration is a fact of ~ and makes its index stale" * doctest::test_suite("slow"))
{
    const Fixture               f = build_fixture("zelph_pidx_declaration", "a0 ~ hub\nhub ~ knows\n");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    const auto classes = [&]
    {
        interactive.process(R"(%(string "CLASSES-" (string/join (sorted (map zelph/name (zelph/closure "a0" "~"))) " ") "."))");
        return last_out_text(collector);
    };

    CHECK(classes().find("CLASSES-hub knows.") != std::string::npos);
    CHECK(index_builds(collector) == 1);

    interactive.process("x knows y");
    CHECK(classes().find("CLASSES--> hub knows.") != std::string::npos);

    fs::remove_all(f.root);
}

// A query builds its patterns as facts, and every identical query issued
// immediately after a .load rebuilt the index. A pattern featuring a variable
// subject does not constitute an entry in any index.
TEST_CASE("predicate index: a question keeps it" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_question");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    CHECK(closure_counts(interactive, collector, kFacts));
    interactive.process("X p a7");
    CHECK(closure_counts(interactive, collector, kFacts));
    CHECK(index_builds(collector) == 1);

    fs::remove_all(f.root);
}

// A SPARQL query is evaluated within a cluster of its own, which is dropped
// alongside the answer. The drop removes the query's patterns associated with
// the predicate, which do not appear in any index, while preserving the index
// constructed before the query.
TEST_CASE("predicate index: a SPARQL question keeps it" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_sparql_question");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");
    interactive.process(".import sparql");

    CHECK(closure_counts(interactive, collector, kFacts));
    for (const char* line : {"sparql", "SELECT ?y ?z WHERE { a7 p ?y . ?y p ?z . }", ""})
        interactive.process(line);
    CHECK(any_output_contains(collector, "No results."));
    CHECK(closure_counts(interactive, collector, kFacts));
    CHECK(index_builds(collector) == 1);

    fs::remove_all(f.root);
}

// On the opposite side of the two scenarios mentioned earlier: what the
// narrowing must not lose.
TEST_CASE("predicate index: a ground fact of the predicate rebuilds it" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_same_fact");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    CHECK(closure_counts(interactive, collector, kFacts));
    interactive.process("b1 p hub");
    CHECK(closure_counts(interactive, collector, kFacts + 1));
    CHECK(index_builds(collector) == 2);

    fs::remove_all(f.root);
}

// The index skips a rule's ground pattern, and claiming the pattern creates no
// fact of its predicate: the claim eliminates the marking, a fact of `~`. Only
// an invalidation tied to the claimed statement's own predicate brings it into
// the next closure.
TEST_CASE("predicate index: claiming a rule's ground pattern of the predicate puts it into the next closure" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_claim");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    interactive.process("(z1 p hub) => (z2 q r)");
    CHECK(closure_counts(interactive, collector, kFacts));
    interactive.process("z1 p hub");
    CHECK(closure_counts(interactive, collector, kFacts + 1));

    // And back: the drop of a cluster that claimed a pattern reasserts the
    // mark, which creates no fact regarding `p` either.
    interactive.process("(z5 p hub) => (z6 q r)");
    interactive.process(".cluster c");
    interactive.process("z5 p hub");
    CHECK(closure_counts(interactive, collector, kFacts + 2));
    interactive.process(".cluster default");
    interactive.process(".cluster-drop c");
    CHECK(closure_counts(interactive, collector, kFacts + 1));

    fs::remove_all(f.root);
}

// Replaces "predicate index: a removed refutation marking puts its fact into
// the next closure", which relied on `¬(z7 p hub)` claiming the rule's pattern
// it refuted: upon deletion of the marking, the fact became data and joined
// the closure. Since a refutation claims its marking, not its fact
// (Zelph::refute), the fact stays the rule's pattern, and the closure skips it
// both before and after the marking is removed. The index follows the claim
// that makes it data. A closure omits both refuted facts and rule patterns,
// and removing a marking continues to invalidate the fact's predicate index
// (forget_refuted).
TEST_CASE("predicate index: a removed refutation marking leaves its pattern out of the closure until it is claimed" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_refutation");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    interactive.process("(z7 p hub) => (z8 q r)");
    interactive.process("¬(z7 p hub)");
    CHECK(closure_counts(interactive, collector, kFacts));

    // The identifier of a fact node is conveyed by `.node`.
    collector.clear();
    interactive.process(".node (z7 p hub) ~ refuted");
    std::string marking;
    for (const auto& event : collector.events())
        if (const std::size_t at = event.text.find("Node ID: "); at != std::string::npos) marking = event.text.substr(at + 9);
    REQUIRE_FALSE(marking.empty());
    interactive.process(".remove " + marking);
    CHECK(closure_counts(interactive, collector, kFacts));

    interactive.process("z7 p hub");
    CHECK(closure_counts(interactive, collector, kFacts + 1));

    fs::remove_all(f.root);
}

// A closure steps along no bookkeeping, both through the index and
// through the direct walk ("path condition: a walk steps along no
// bookkeeping, as a condition matches none"). The index cannot omit the
// bookkeeping, because zelph/sources reads it and lists bookkeeping, thus the
// walk over the index omits the step that a record's marking takes. A hub
// on the walk exhausts the scan budget: forward, hub arrives at `refuted`
// via the record of a contradiction concerning the set it names, and
// backward, `refuted` arrives at the record through its marking. The
// statement `hub ~ refuted` constitutes a step someone wrote, and it is
// walked.
TEST_CASE("predicate index: a closure through the index steps along no record of a contradiction" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_bookkeeping");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");
    process_lines(interactive, "x p y\nx q y\nhub ~ {(x p y) (x q y)}\n(A p B, A q B) => !\n.run\n");

    const auto reached = [&](const std::string& call)
    {
        collector.clear();
        interactive.process("%(each n " + call + " (zelph/out (string \"REACHED \" (string/format \"%q\" n))))");
        std::vector<std::string> nodes;
        for (const auto& event : collector.events())
            if (const std::string line = normalize(event.text); line.rfind("REACHED ", 0) == 0) nodes.push_back(line.substr(8));
        std::sort(nodes.begin(), nodes.end());
        CHECK(index_builds(collector) == 1);
        return nodes;
    };

    const std::vector<std::string> forward = reached(R"((zelph/closure "hub" "~"))");
    CHECK(forward.size() == 1);
    for (const std::string& node : forward)
        CHECK(node.starts_with("<zelph/node {("));

    interactive.process("hub ~ refuted");
    CHECK(reached(R"((zelph/closure-sources "refuted" "~"))") == std::vector<std::string>{"<zelph/node hub>"});

    fs::remove_all(f.root);
}

// The identical two removals that cost the index of every predicate: one via
// the cascade of remove_node, one due to a dropped cluster.
TEST_CASE("predicate index: removing a fact of another predicate keeps it" * doctest::test_suite("slow"))
{
    const Fixture               f = fixture_copy("zelph_pidx_removal");
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.bin.string() + "\"");

    interactive.process("x likes y");
    CHECK(closure_counts(interactive, collector, kFacts));

    SUBCASE(".remove")
    {
        interactive.process(".remove x");
    }
    SUBCASE("a dropped cluster")
    {
        process_lines(interactive, ".cluster c\nw likes v\n.cluster default\n.cluster-drop c");
    }
    CHECK(closure_counts(interactive, collector, kFacts));
    CHECK(index_builds(collector) == 1);

    fs::remove_all(f.root);
}

// A self-fact represents a single-step loop, and the one-or-more closure of
// its node contains the node itself. A closure is answered in one of two ways
// -- via a direct walk beneath the scan budget, or through an index above it
// -- with each way reading a relation independently. The index read the loop,
// whereas the walk did not, meaning that within the same graph and over the
// same predicate, `hub p hub` made hub part of its own closure, while `k p k`
// left k out of its. Asked of both ways, during the session that builds the
// sidecar and during the one that loads it.
TEST_CASE("predicate index: a loop of one step is a path, through the index as through the direct walk")
{
    const Fixture f = build_fixture("zelph_pidx_loop_test", "hub p hub\nk p k\n");

    for (const char* sidecar : {"Building adjacency index", "Loaded adjacency index"})
    {
        CAPTURE(sidecar);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + f.bin.string() + "\"");

        // hub carries additional facts beyond what the scan budget allows:
        // the index.
        interactive.process(R"(%(string "HUB-" (length (zelph/closure "hub" "p"))))");
        interactive.process(R"(%(string "HUB-SOURCES-" (length (zelph/closure-sources "hub" "p"))))");
        CHECK(any_event_contains(collector, sidecar));
        CHECK(any_output_contains(collector, "HUB-1"));
        CHECK(any_output_contains(collector, "HUB-SOURCES-" + std::to_string(kFacts + 1)));

        // k holds a single fact: the direct walk.
        interactive.process(R"(%(string "K-" (length (zelph/closure "k" "p"))))");
        interactive.process(R"(%(string "K-SOURCES-" (length (zelph/closure-sources "k" "p"))))");
        CHECK(any_output_contains(collector, "K-1"));
        CHECK(any_output_contains(collector, "K-SOURCES-1"));
    }

    fs::remove_all(f.root);
}
