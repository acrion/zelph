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

// A fact whose SUBJECT IS its predicate.
//
// insert_fact writes _left[fact] = {subject, predicate}, so when the two are
// the same node both roles collapse into a single entry and the back edge can
// no longer tell them apart. Every candidate filter in the engine read that
// single entry as "subject, hence not the predicate", which left such a fact
// undecomposable: stored, printable and explainable, but matched by nothing.
//
// This is not an exotic shape. `~ ~ ->` -- "the category relation is itself a
// relation type" -- is created by zelph in EVERY network, and it is what
// licenses reading `~` as a predicate at all: relation_type_set() is built
// from the `X ~ ->` facts, so without this one a statement whose predicate is
// `~` has no readable predicate and `S ~ O` answers nothing whatsoever.

#include "test_helpers.hpp"

#include "io/output.hpp"
#include "network/zelph.hpp"

#include <filesystem>

using namespace zelph::test;
using namespace zelph::network;

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }
} // namespace

TEST_CASE("self-predicate: the core fact `~ ~ ->` answers like every other declaration")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");

        collector.clear();
        interactive.process("S ~ O");

        // The declarations of the core vocabulary, and `rel` from the line
        // above -- but the one about `~` itself used to be missing, so the
        // engine's own criterion for "is a predicate" excluded the predicate
        // it applies that criterion with.
        CHECK(answers_contain(collector, "~ ~ ->"));
        CHECK(answers_contain(collector, "rel ~ ->"));
        CHECK(answers_contain(collector, "cons ~ ->"));

        // Same through a bound object, which takes the anchored candidate
        // path rather than the full-relation scan.
        collector.clear();
        interactive.process("S ~ ->");
        CHECK(answers_contain(collector, "~ ~ ->")); });
}

TEST_CASE("self-predicate: a user fact with subject == predicate is queryable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("p p q");

        // All three shapes: free subject and object (full scan), bound
        // subject and bound object (both anchored).
        for (const char* query : {"S p O", "p p O", "S p q"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(answers_contain(collector, "p p q"));
        }

        // It is an axiom, and .explain always said so even while nothing
        // could match it.
        collector.clear();
        interactive.process(".explain (p p q)");
        CHECK(any_output_contains(collector, "axiom")); });
}

TEST_CASE("self-predicate: a rule fires on a subject == predicate fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("p p q");
        collector.clear();
        interactive.process("(X p Y) => (X found Y)");
        interactive.process(".run");

        collector.clear();
        interactive.process("S found O");
        CHECK(answers_contain(collector, "p found q")); });
}

TEST_CASE("self-predicate: an ordinary fact is still not read as its own predicate")
{
    // The exemption keys on the fact's outgoing adjacency collapsing to one
    // entry. For `a rel b` that adjacency is {a, rel}, so `a` stays the
    // subject and asking for facts of predicate `a` must find none -- red if
    // the exemption were widened to "relation is bidirectional".
    Zelph      z(null_handler());
    const Node a   = z.node("a");
    const Node b   = z.node("b");
    const Node rel = z.node("rel");

    const Node f = z.fact(a, rel, {b});

    CHECK(z.get_facts_of_predicate(rel).count(f) == 1);
    CHECK(z.get_facts_of_predicate(a).count(f) == 0);

    // ... while the collapsed shape is found from both ends.
    const Node op = z.node("op");
    const Node g  = z.fact(op, op, {b});
    CHECK(z.get_facts_of_predicate(op).count(g) == 1);

    zelph::network::adjacency_set anchored;
    z.collect_anchored_facts(op, op, anchored); // subject-driven
    CHECK(anchored.count(g) == 1);
    z.collect_anchored_facts(b, op, anchored); // object-driven
    CHECK(anchored.count(g) == 1);

    CHECK(z.get_fact_objects(op, op).count(b) == 1);
    CHECK(z.get_fact_subjects(op, b).count(op) == 1);
}

TEST_CASE("self-predicate: a subject == predicate fact survives save and load")
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "zelph_test_self_predicate.bin").string();

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("p p q");
        interactive.process(".save " + path);
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path);
    interactive.process(".auto-run");

    collector.clear();
    interactive.process("S p O");
    CHECK(answers_contain(collector, "p p q"));

    collector.clear();
    interactive.process("S ~ O");
    CHECK(answers_contain(collector, "~ ~ ->"));

    std::filesystem::remove(path);
}
