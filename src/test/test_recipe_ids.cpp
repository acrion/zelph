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

#include "io/output.hpp"
#include "network/reasoning.hpp"
#include "network/zelph.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace zelph::network;

// ---------------------------------------------------------------------------
// The ids for collections built by rules. A collection a rule builds IS its
// recipe: its id is computed from the template collection and the binding,
// thus building it again locates the node via its id, and no lookup ever
// occurs based on the collection's members. The origin is encoded within
// the id as well -- either the rule's own collection (the template class)
// or a term built by a firing (a value recipe) -- hence distinguishing
// between them requires no inspection of any fact within the graph.
//
// The layout is described here using literal bit patterns instead of the
// constants defined in network.hpp: a test relying on the code's own
// constants would match any value assigned to them.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }

    constexpr Node written_payload = 0x07FFFFFFFFFFFFFFull; // bits 58..0

    // How many rules ".list-rules" lists.
    std::size_t listed_rules(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(".list-rules");
        return static_cast<std::size_t>(std::count_if(collector.events().begin(), collector.events().end(), [](const auto& e)
                                                      { return zelph::test::normalize(e.text).find("=>") != std::string::npos; }));
    }

    // Inputs of every id class, along with a spread of 64-bit values derived
    // from a fixed linear congruential sequence, ensuring that the masks
    // observe numerous hash outputs.
    std::vector<Node> sample_nodes()
    {
        std::vector<Node> out{1,
                              12345,
                              0x1FFFFFFFFFFFFFFFull, // the last counter
                              0x2000000000000000ull, // a value recipe
                              0x3000000000000005ull, // a written template
                              0x3800000000000007ull, // a construction's recipe
                              0x4000000000000003ull, // a hash
                              0x7FFFFFFFFFFFFFFFull,
                              0xFFFFFFFFFFFFFFFEull}; // a variable
        Node              x = 0x0123456789ABCDEFull;
        for (int i = 0; i < 500; ++i)
        {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            out.push_back(x);
        }
        return out;
    }
} // namespace

TEST_CASE("recipe ids: the classes are told apart by the top bits")
{
    // Each reader that takes a non-hash value as an atom or a collection
    // must continue to take a recipe for a collection, and neither a fact
    // nor a set constant may land on one: a recipe is never a hash, never a
    // variable, and never a counter, which create() hands out below 2^61.
    CHECK_FALSE(Zelph::is_recipe(1));
    CHECK_FALSE(Zelph::is_recipe(0x1FFFFFFFFFFFFFFFull));
    CHECK(Zelph::is_value_recipe(0x2000000000000000ull));
    CHECK(Zelph::is_value_recipe(0x2FFFFFFFFFFFFFFFull));
    CHECK_FALSE(Zelph::is_template_id(0x2FFFFFFFFFFFFFFFull));
    CHECK(Zelph::is_template_id(0x3000000000000000ull));
    CHECK(Network::is_written_template(0x3000000000000000ull));
    CHECK(Network::is_written_template(0x37FFFFFFFFFFFFFFull));
    CHECK(Zelph::is_template_id(0x3800000000000000ull));
    CHECK_FALSE(Network::is_written_template(0x3800000000000000ull));
    CHECK(Zelph::is_template_id(0x3FFFFFFFFFFFFFFFull));
    CHECK_FALSE(Zelph::is_value_recipe(0x3FFFFFFFFFFFFFFFull));
    CHECK_FALSE(Zelph::is_recipe(0x4000000000000000ull));
    CHECK_FALSE(Zelph::is_recipe(0x8000000000000000ull));
    CHECK_FALSE(Zelph::is_recipe(0xFFFFFFFFFFFFFFFFull));

    const std::vector<Node> nodes              = sample_nodes();
    const Node              v                  = 0xFFFFFFFFFFFFFFF0ull;
    int                     value_wrong        = 0;
    int                     construction_wrong = 0;
    for (const Node t : nodes)
    {
        for (const Node key : {Network::recipe_key({}), Network::recipe_key({{v, t}}), t})
        {
            const Node value = Network::recipe_id(t, key, false);
            if ((value >> 60) != 0x2 || !Zelph::is_recipe(value) || !Zelph::is_value_recipe(value)
                || Zelph::is_template_id(value) || Zelph::is_hash(value) || Zelph::is_var(value))
                ++value_wrong;

            // A construction's recipe: the template class having bit 59
            // set, meaning it is never considered a written template.
            const Node construction = Network::recipe_id(t, key, true);
            if ((construction >> 59) != 0x7 || !Zelph::is_recipe(construction) || !Zelph::is_template_id(construction)
                || Network::is_written_template(construction) || Zelph::is_value_recipe(construction)
                || Zelph::is_hash(construction) || Zelph::is_var(construction))
                ++construction_wrong;
        }
    }
    CHECK(value_wrong == 0);
    CHECK(construction_wrong == 0);
}

TEST_CASE("recipe ids: the id is a function of template, key and mode")
{
    // Building a rule's collection again under the identical binding must
    // resolve to the same node, without invoking a lookup -- and two
    // bindings that differ must yield two distinct collections, otherwise
    // `(X p Y) => (X likes @{(Z q Y)})` using `a p k` and `b p k` would
    // result in a and b sharing one collection.
    const Node t1 = 101;
    const Node t2 = 102;
    const Node x  = 0xFFFFFFFFFFFFFFF0ull; // two variables
    const Node y  = 0xFFFFFFFFFFFFFFF1ull;
    const Node a  = 201;
    const Node b  = 202;

    const Node key_xa = Network::recipe_key({{x, a}});
    CHECK(Network::recipe_key({{x, a}}) == key_xa);
    CHECK(Network::recipe_key({{x, b}}) != key_xa);
    CHECK(Network::recipe_key({{y, a}}) != key_xa);
    CHECK(Network::recipe_key({}) != key_xa);
    // A binding does not constitute the set of its values: the
    // assignment of a specific value to a particular variable is
    // integral to the key.
    CHECK(Network::recipe_key({{x, a}, {y, b}}) != Network::recipe_key({{x, b}, {y, a}}));

    for (const bool construction : {false, true})
    {
        const Node id = Network::recipe_id(t1, key_xa, construction);
        CHECK(Network::recipe_id(t1, key_xa, construction) == id);
        CHECK(Network::recipe_id(t2, key_xa, construction) != id);
        CHECK(Network::recipe_id(t1, Network::recipe_key({{x, b}}), construction) != id);
        CHECK(Network::recipe_id(t1, key_xa, !construction) != id);
    }
}

TEST_CASE("recipe ids: a bucket's term is one value recipe for every binding")
{
    // The collection a rule writes into (its bucket) meets the data via a
    // single term common to every firing of the rule: a value, keyed by no
    // variable, and distinct from the id that a template acquires elsewhere
    // under a binding or within a construction.
    const Node bucket = 103;
    const Node x      = 0xFFFFFFFFFFFFFFF0ull;

    const Node term = Zelph::bucket_term_id(bucket);
    CHECK(Zelph::is_value_recipe(term));
    CHECK(Zelph::bucket_term_id(bucket) == term);
    CHECK(Zelph::bucket_term_id(104) != term);
    CHECK(Network::recipe_id(bucket, Network::recipe_key({{x, 201}}), false) != term);
    CHECK(Network::recipe_id(bucket, Network::recipe_key({{x, 202}}), false) != term);
    CHECK(Network::recipe_id(bucket, Network::recipe_key({}), true) != term);
}

TEST_CASE("recipe ids: a written template takes the next counter value as its payload")
{
    // A collection written during the writing of a rule is assigned an id
    // of the template class whose payload is a counter value, ensuring it
    // remains unique in the same manner as a counter. The counter slot it
    // consumes remains vacant, and subsequent counters are positioned
    // above it.
    Network    net;
    const Node c1 = net.create();
    const Node c2 = net.create();
    CHECK(c2 > c1);

    const Node t1 = net.create_written_template();
    CHECK(net.exists(t1));
    CHECK(Network::is_written_template(t1));
    CHECK(Network::is_template_id(t1));
    CHECK_FALSE(Network::is_value_recipe(t1));
    CHECK((t1 >> 59) == 0x6); // bits 63..59 = 00110: bit 59 clear
    const Node p1 = t1 & written_payload;
    CHECK(p1 > c2);
    CHECK_FALSE(net.exists(p1));

    const Node c3 = net.create();
    CHECK(c3 > p1);

    const Node t2 = net.create_written_template();
    CHECK(t2 != t1);
    CHECK(Network::is_written_template(t2));
    CHECK((t2 & written_payload) > c3);
}

TEST_CASE("recipe ids: a written template skips a payload whose id exists")
{
    // A load writes ids exactly as they are and takes the counter from the
    // file, meaning a load that merges a file into a session may result in
    // the counter being lower than a written template the session already
    // holds. create(Node) leaves the node in the state that such a load
    // produces, with the counter remaining unchanged.
    Network    net;
    const Node held = 0x3000000000000000ull | 1;
    net.create(held);

    const Node t = net.create_written_template();
    CHECK(t != held);
    CHECK(Network::is_written_template(t));
    CHECK((t & written_payload) == 2);
    CHECK(net.exists(held));
}

TEST_CASE("recipe ids: a written template is recorded in the cluster it was written in")
{
    // A rule is written within a scratch cluster, which is dropped when
    // the rule proves to be a twin of an existing one, and its templates
    // must go with the scratch; `.cluster-drop` likewise takes back a
    // written template. A cluster drops solely what it has recorded.
    Network net;
    net.set_active_cluster("scratch");
    const Node t = net.create_written_template();
    net.deactivate_cluster();

    const std::vector<Node> recorded = net.cluster_nodes("scratch");
    CHECK(std::find(recorded.begin(), recorded.end(), t) != recorded.end());
}

TEST_CASE("template scope: a collection written in it is a written template, a condition set is not")
{
    // While a rule is being authored, any collection written within it
    // becomes an integral component of that rule's text, and its id says so
    // for good; outside the scope, a collection functions as a value. A
    // literal containing a variable member reverts to a collection and acts
    // as a template using it, whereas a ground literal remains a set
    // constant. The condition set of a rule keeps a counter id inside the
    // scope: its tag specifies its nature regardless of position, ensuring
    // that typed rules keep the ids they have always possessed.
    Zelph      z(null_handler());
    const Node a = z.node("a");
    const Node v = z.var();

    const Node before = z.collection({a});

    z.enter_template_scope();
    const Node written    = z.collection({a});
    const Node fallback   = z.set({v});
    const Node constant   = z.set({a});
    const Node conditions = z.conjunction_collection({a});
    const Node reopened   = z.collection({a});
    z.enter_template_scope();
    const Node nested = z.collection({a});
    z.leave_template_scope();
    const Node enclosing = z.collection({a});
    z.leave_template_scope();

    const Node after = z.collection({a});

    CHECK_FALSE(Network::is_recipe(before));
    CHECK(Network::is_written_template(written));
    CHECK(Network::is_written_template(fallback));
    CHECK(Zelph::is_hash(constant));
    CHECK_FALSE(Network::is_recipe(conditions));
    CHECK_FALSE(Zelph::is_hash(conditions));
    CHECK(Network::is_written_template(reopened));
    CHECK(Network::is_written_template(nested));
    CHECK(Network::is_written_template(enclosing));
    CHECK_FALSE(Network::is_recipe(after));
}

TEST_CASE("template scope: the collections written in it are listed until the outermost scope closes")
{
    // zelph/rule-text marks the memberships of the collections written for
    // the rule it writes: these are the collections written after its own
    // scope was initiated, even if nested within another scope. A
    // condition set does not constitute such a collection.
    Zelph      z(null_handler());
    const Node a = z.node("a");

    z.enter_template_scope();
    const Node        outer = z.collection({a});
    const std::size_t mark  = z.template_scope_mark();
    z.enter_template_scope();
    const Node inner = z.collection({a});
    z.conjunction_collection({a});

    CHECK(z.template_scope_collections_since(mark) == std::vector<Node>{inner});
    CHECK(z.template_scope_collections_since(0) == std::vector<Node>{outer, inner});

    z.leave_template_scope();
    CHECK(z.template_scope_collections_since(0).size() == 2);
    z.leave_template_scope();
    CHECK(z.template_scope_collections_since(0).empty());
}

TEST_CASE("template scope: it belongs to the thread that opened it, on the network it was opened on")
{
    // Whether a collection belongs to a rule's text says who wrote it, thus
    // it cannot depend on concurrent actions by another thread. A collection
    // generated by a different thread during this thread's rule writing is
    // considered data, just as one created on a separate network is; any
    // attempt by another thread to write membership into this rule's
    // collection is rejected, as any write from outside the rule is. The
    // scope was defined as one depth per network: a Janet thread's
    // collection got a template identifier while the script thread resided
    // within zelph/rule, and a rule that bound it read its empty data term.
    Zelph      z(null_handler());
    Zelph      other(null_handler());
    const Node a = z.node("a");

    const Zelph::TemplateScope scope(z);
    const Node                 own = z.collection({a});

    Node        from_thread = 0;
    std::string refusal;
    std::thread writer([&]
                       {
                           from_thread = z.collection({a});
                           try
                           {
                               z.fact(z.node("b"), z.core.PartOf, {own});
                           }
                           catch (const std::runtime_error& e)
                           {
                               refusal = e.what();
                           } });
    writer.join();

    CHECK(Network::is_written_template(own));
    CHECK_FALSE(Network::is_recipe(from_thread));
    CHECK(refusal.find("fixed once the rule is written") != std::string::npos);
    CHECK_FALSE(z.check_fact(z.node("b"), z.core.PartOf, {own}).is_known());
    CHECK_FALSE(Network::is_recipe(other.collection({other.node("a")})));
    CHECK(z.template_scope_collections_since(0) == std::vector<Node>{own});
}

TEST_CASE("template ids survive a save and a load, and the next one written is fresh")
{
    // The identifier of a template is its authorship, so saving records it
    // exactly as it stands, and loading restores it back with the counter
    // its payload came from: a template stays a template, a value stays a
    // value, and a template written following the load is a new one.
    const auto path        = std::filesystem::temp_directory_path() / "zelph_template_ids.bin";
    Node       template_id = 0;
    Node       value       = 0;
    {
        Zelph      z(null_handler());
        const Node a = z.node("a");
        const Node x = z.var();
        const Node y = z.var();

        value = z.collection({a});
        z.enter_template_scope();
        template_id = z.collection({a});
        z.leave_template_scope();

        z.fact(z.fact(x, z.node("p"), {y}), z.core.Causes, {z.fact(x, z.node("likes"), {value})});
        z.fact(z.fact(x, z.node("q"), {y}), z.core.Causes, {z.fact(x, z.node("likes"), {template_id})});
        z.save_to_file(path.string());
    }

    Zelph z(null_handler());
    z.load_from_file(path.string());

    CHECK(z.exists(template_id));
    CHECK(z.is_rule_template(template_id));
    CHECK(z.exists(value));
    CHECK_FALSE(z.is_rule_template(value));

    z.enter_template_scope();
    const Node next = z.collection({z.node("b")});
    z.leave_template_scope();
    CHECK(Network::is_written_template(next));
    CHECK((next & written_payload) > (template_id & written_payload));

    std::filesystem::remove(path);
}

namespace zelph::test
{
    // Binary files from older_networks/*.bin are integrated directly into
    // this compiled executable via embed_files.cmake.
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

TEST_CASE("load: a network saved before template ids says that the collections in its rules are data")
{
    // The file `older_networks/rule_collection.bin` was saved by an engine
    // before template ids, following
    //   (X p Y) => (X likes @{c d})
    //   a p b
    // The engine wrote the rule's `@{c d}` beneath a counter id, just as it
    // wrote every collection, and named that specific node within the data:
    // `a likes @{c d}`. When loaded, the node functions as a value, so the
    // rule keeps the older behaviour -- a fresh binding refers to the
    // identical node -- and the rule typed again constitutes a second rule,
    // with its literal serving as a template. Only rebuilding the network
    // from its scripts alters this state, and the load explicitly indicates
    // this. Loading such a file is the way a user reaches this state: this
    // engine writes a typed rule's collections as templates.
    zelph::test::run_both_modes([](auto& collector, auto& interactive)
                                {
        const auto& networks = zelph::test::older_networks();
        const auto  network  = networks.find("rule_collection");
        REQUIRE(network != networks.end());

        const auto path = std::filesystem::temp_directory_path() / "zelph_older_rule_collection.bin";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
        }
        interactive.process(".load " + path.string());
        std::filesystem::remove(path);
        // The note specifies solely what holds across all networks it is
        // printed for: the collections within its rules are data in this
        // context, and a rebuild based on the scripts gives the rules
        // collections of their own. It claims no preserved behaviour, because
        // the older engine built a literal that a binding makes ground --
        // `@{Y}` -- as a set constant per binding, where a firing here names
        // the rule's own collection, including the variable.
        const std::string note = "Note: rules in this network were saved by an older zelph, and their collections are data here; "
                                 "rebuild the network from its scripts before reasoning over it.";
        CHECK(std::ranges::any_of(collector.events(), [&note](const auto& event)
                                  { return event.channel == zelph::io::OutputChannel::Out && event.text == note; }));

        collector.clear();
        interactive.process("S likes O");
        CHECK(zelph::test::collect_answers(collector) == std::vector<std::string>{"a likes @{c d}"});

        auto* const z         = interactive.graph();
        const auto  liked_by  = [z](const std::string& subject)
        {
            for (const Node f : z->get_right(z->get_node(subject)))
            {
                if (z->predicate_of(f) != z->get_node("likes")) continue;
                adjacency_set objects;
                if (z->parse_fact(f, objects) == z->get_node(subject) && objects.size() == 1) return *objects.begin();
            }
            return Node{0};
        };

        interactive.process("d p e");
        interactive.run(true, false, false);
        REQUIRE(liked_by("a") != 0);
        CHECK(liked_by("d") == liked_by("a"));
        CHECK_FALSE(z->is_rule_template(liked_by("a")));

        interactive.process("(X p Y) => (X likes @{c d})");
        CHECK(listed_rules(collector, interactive) == 2); });
}

TEST_CASE("load: a network saved before template ids says so again after this engine saved it")
{
    // A save writes each id precisely as it stands, so the rules the older
    // engine saved keep their counter collections, and the older behaviour
    // with them, across every file this engine writes for the network. The
    // note belongs to each complete file load: a network whose load printed
    // the note gets saved without the template-id field, and the next load
    // reads its rules again. The second round trip loads a file generated by
    // this engine, and its save must omit the field too, otherwise the third
    // load remains silent.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("rule_collection");
    REQUIRE(network != networks.end());

    const auto dir   = std::filesystem::temp_directory_path();
    const auto older = dir / "zelph_older_resaved_0.bin";
    const auto once  = dir / "zelph_older_resaved_1.bin";
    const auto twice = dir / "zelph_older_resaved_2.bin";
    {
        std::ofstream out(older, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }

    // Loads a file into a dedicated session, stores the network if a
    // target is named, and indicates if the load printed the note.
    const auto load_and_save = [](const std::filesystem::path& from, const std::filesystem::path& to)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + from.string());
        const bool noted = zelph::test::any_event_contains(collector, "saved by an older zelph");
        if (!to.empty()) interactive.process(".save " + to.string());
        return noted;
    };

    CHECK(load_and_save(older, once));
    CHECK(load_and_save(once, twice));
    CHECK(load_and_save(twice, {}));

    std::filesystem::remove(older);
    std::filesystem::remove(once);
    std::filesystem::remove(twice);
}

TEST_CASE("load: an older network whose rules hold no literal of their own says nothing")
{
    // The file `older_networks/data_collections.bin` was saved by an engine
    // before template ids, following
    //   (X p Y, Y p Z) => (X pp Z)
    //   (X q Y) => (X in mammals)
    //   d has @{x}
    //   (S has C) => ((X r Y) => (X in C))
    //   z q y
    //   w r v
    // The rules hold collections, yet none that the older engine wrote as a
    // rule's literal: a condition set, the named node `mammals`, and `@{x}`,
    // a data collection the generator bound into the rule it wrote, which
    // holds a claim, the rule's own statement `X in @{x}` and what the rule
    // derived. Their behaviour remains unchanged from before, so there is
    // nothing to rebuild. The note pertains to a collection within a rule's
    // text whose membership is written as the rule's literal -- either
    // marked as a rule pattern or holding a variable, and no statement of a
    // rule -- and only a file missing the template-id field is asked for
    // one.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("data_collections");
    REQUIRE(network != networks.end());

    const auto path = std::filesystem::temp_directory_path() / "zelph_older_data_collections.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path.string());
    std::filesystem::remove(path);
    CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 4);
}

TEST_CASE("load: a partial view of an older network says nothing about its rules")
{
    // A view is not reasoned over -- .run refuses -- and might be missing
    // the rules, the memberships, and the names that the note depends on,
    // thus only a whole-file load is asked for it. This view holds all of
    // older_networks/rule_collection.bin, whose whole-file load does print
    // the note.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("rule_collection");
    REQUIRE(network != networks.end());

    const auto path = std::filesystem::temp_directory_path() / "zelph_older_rule_collection_view.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load-partial " + path.string());
    std::filesystem::remove(path);
    CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 1);
}

TEST_CASE("load: a network saved after a partial load replaced an older one says nothing about its rules")
{
    // A whole-file load that finds an older engine's rules makes every
    // network save omit the template-id field. A partial load discards that
    // network along with its rules, so what gets saved afterwards includes
    // the field once more. The network saved here holds only a rule this
    // engine built over a collection built by a Janet program before the
    // rule, which the scan interprets as an older engine's literal: if saved
    // without the field, it would be noted on every later load. A whole-file
    // load of the file displayed by the view leads from the view back to a
    // network capable of being saved.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("rule_collection");
    REQUIRE(network != networks.end());

    const auto dir   = std::filesystem::temp_directory_path();
    const auto older = dir / "zelph_older_replaced_0.bin";
    const auto own   = dir / "zelph_older_replaced_1.bin";
    const auto saved = dir / "zelph_older_replaced_2.bin";
    {
        std::ofstream out(older, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(R"js(%(def C (zelph/collection (zelph/fact 'Z "q" 'Y))))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "likes" C)))js");
        REQUIRE(listed_rules(collector, interactive) == 1);
        interactive.process(".save " + own.string());
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + older.string());
        REQUIRE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
        interactive.process(".load-partial " + own.string());
        interactive.process(".load " + own.string());
        interactive.process(".save " + saved.string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + saved.string());
    std::filesystem::remove(older);
    std::filesystem::remove(own);
    std::filesystem::remove(saved);
    CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 1);
}

TEST_CASE("load: a network saved after .remove-rules took an older engine's rules says nothing about its rules")
{
    // Eliminating each rule also erases the older engine's rules alongside
    // it, causing the network to be saved with the template-id field once
    // more. The rule built following the removal belongs to this engine,
    // over a collection that a Janet program created before the rule, which
    // the scan interprets as an older engine's literal: if saved without
    // the field, it would be noted on every later load.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("rule_collection");
    REQUIRE(network != networks.end());

    const auto dir   = std::filesystem::temp_directory_path();
    const auto older = dir / "zelph_older_removed_0.bin";
    const auto saved = dir / "zelph_older_removed_1.bin";
    {
        std::ofstream out(older, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + older.string());
        REQUIRE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
        interactive.process(".remove-rules");
        REQUIRE(listed_rules(collector, interactive) == 0);
        interactive.process(R"js(%(def C (zelph/collection (zelph/fact 'Z "q" 'Y))))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "likes" C)))js");
        REQUIRE(listed_rules(collector, interactive) == 1);
        interactive.process(".save " + saved.string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + saved.string());
    std::filesystem::remove(older);
    std::filesystem::remove(saved);
    CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 1);
}

TEST_CASE("load: an older engine's rule that a statement mentions outlasts .remove-rules, and so does the note")
{
    // The file `older_networks/mentioned_rule.bin` was saved by an engine
    // before template ids, after
    //   (X p Y) => (X likes @{Y})
    //   ((X q Y) => (X hates @{Y})) is noted
    // The `.remove-rules` command eliminates the rule in force, not the rule
    // referenced by the statement, whose `@{Y}` keeps its counter id and
    // preserves the membership established by the older engine as the rule's
    // literal. The network saved after the removal continues to maintain
    // that text, thus its next load gives the note, just as the load of the
    // older file does: the rules are processed once more following the
    // removal, not treated as having vanished.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("mentioned_rule");
    REQUIRE(network != networks.end());

    const auto dir   = std::filesystem::temp_directory_path();
    const auto older = dir / "zelph_older_mentioned_0.bin";
    const auto saved = dir / "zelph_older_mentioned_1.bin";
    {
        std::ofstream out(older, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + older.string());
        REQUIRE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
        REQUIRE(listed_rules(collector, interactive) == 1);
        interactive.process(".remove-rules");
        REQUIRE(listed_rules(collector, interactive) == 0);
        interactive.process(".save " + saved.string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + saved.string());
    std::filesystem::remove(older);
    std::filesystem::remove(saved);
    CHECK(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 0);
}

TEST_CASE("load: a network this engine saved says nothing about its rules' collections")
{
    // Each file this engine writes includes in its header a declaration
    // stating it was produced using template ids, and such a file loads
    // without the note, regardless of what its rules hold: typed literals,
    // a generator's, a data collection that a generated rule names and
    // writes `X in @{x}` into, a named node.
    const auto path = std::filesystem::temp_directory_path() / "zelph_rule_collections.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        zelph::test::process_lines(interactive, R"(
(X p Y) => (X likes @{c d})
(X p Y, Y p Z) => (X pp Z)
(X q Y) => (X in mammals)
(K is on) => ((X s Y) => (X t @{e}))
d has @{x}
(S has C) => ((X r Y) => (X in C))
a p b
z q y
k is on
)");
        interactive.run(true, false, false);
        REQUIRE(listed_rules(collector, interactive) == 7);
        interactive.process(".save " + path.string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path.string());
    std::filesystem::remove(path);
    CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 7);
}

TEST_CASE("load: a rule over a collection built outside every rule scope is not taken for an older engine's")
{
    // A Janet program that builds a collection before it builds the rule
    // makes a value under a counter id, just as an older engine made every
    // collection, and its member `(Z q Y)` holds a variable: within
    // a rule's text, this is precisely how an older engine's literal
    // appears. The C ABI and Rust build every collection of a rule in this
    // manner. Only the template-id field in the file tells the two apart;
    // when read solely from the graph, this network would carry the note
    // across every load.
    const auto path    = std::filesystem::temp_directory_path() / "zelph_rule_over_value.bin";
    const auto resaved = std::filesystem::temp_directory_path() / "zelph_rule_over_value_resaved.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(R"js(%(def C (zelph/collection (zelph/fact 'Z "q" 'Y))))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "likes" C)))js");
        REQUIRE(listed_rules(collector, interactive) == 1);
        interactive.process(".save " + path.string());
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + path.string());
        std::filesystem::remove(path);
        CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
        CHECK(listed_rules(collector, interactive) == 1);
        interactive.process(".save " + resaved.string());
    }

    // A file this engine wrote, once loaded, never marks the network as
    // holding rules from an older engine, thus saving it again keeps the
    // template-id field, and the next load does not ask the scan, which
    // would otherwise interpret this rule as originating from an older
    // engine.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + resaved.string());
    std::filesystem::remove(resaved);
    CHECK_FALSE(zelph::test::any_event_contains(collector, "saved by an older zelph"));
    CHECK(listed_rules(collector, interactive) == 1);
}

TEST_CASE("template scope: what the parser writes as rule text has template ids")
{
    // The parser builds a rule inside zelph/dedup-rule, and another rule
    // that a different statement mentions inside zelph/rule-text: the
    // collections either one writes are templates. The typed rule's
    // condition set keeps a counter id, just as a collection written as
    // data does.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("(X p Y, X r Y) => (X likes @{c})");
    interactive.process("((A q B) => (A has @{d})) is noted");
    interactive.process("e owns @{f}");
    const auto* const z = interactive.graph();

    // The object of the consequence of `rule`.
    const auto consequence_object = [z](const Node rule)
    {
        adjacency_set consequences;
        z->parse_fact(rule, consequences);
        REQUIRE(consequences.size() == 1);
        adjacency_set objects;
        z->parse_fact(*consequences.begin(), objects);
        REQUIRE(objects.size() == 1);
        return *objects.begin();
    };

    Node typed     = 0;
    Node mentioned = 0;
    for (const Node rule : z->get_left(z->core.Causes))
    {
        if (z->predicate_of(rule) != z->core.Causes) continue;
        (z->is_mentioned(rule) ? mentioned : typed) = rule;
    }
    REQUIRE(typed != 0);
    REQUIRE(mentioned != 0);

    adjacency_set unused;
    CHECK(Network::is_written_template(consequence_object(typed)));
    CHECK(Network::is_written_template(consequence_object(mentioned)));
    const Node conditions = z->parse_fact(typed, unused);
    CHECK(z->is_condition_set(conditions));
    CHECK_FALSE(Network::is_recipe(conditions));

    Node data = 0;
    for (const adjacency_set& side : {z->get_left(z->get_node("e")), z->get_right(z->get_node("e"))})
    {
        for (const Node f : side)
        {
            if (z->predicate_of(f) != z->get_node("owns")) continue;
            adjacency_set objects;
            if (z->parse_fact(f, objects) == z->get_node("e") && objects.size() == 1) data = *objects.begin();
        }
    }
    REQUIRE(data != 0);
    CHECK_FALSE(Network::is_recipe(data));
    CHECK_FALSE(Zelph::is_hash(data));
}

TEST_CASE("template scope: what zelph/rule's argument forms write has template ids")
{
    // The zelph/rule macro runs its argument forms within zelph/build-rule,
    // which opens the template scope similarly to how the parser's build
    // does, ensuring that any collection written by those forms is the
    // rule's own. A collection that the program constructed before the
    // call, and another provided to zelph/rule*, whose arguments are
    // evaluated before execution, are created as data and keep counter ids;
    // likewise, the condition set built by zelph/rule* also keeps counter
    // ids. A rule authored manually within zelph/build-rule writes its
    // collections into the same scope.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "likes" (zelph/collection "c"))))js");
    interactive.process(R"js(%(def D (zelph/collection "d")))js");
    interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "q" 'Y)] (zelph/fact 'X "loves" D)))js");
    interactive.process(R"js(%(zelph/rule* [(zelph/fact 'X "r" 'Y)] (zelph/fact 'X "has" (zelph/collection "e"))))js");
    interactive.process(R"js(%(zelph/build-rule (fn [] (let [condition (zelph/collection (zelph/fact 'X "s" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "owns" (zelph/collection "f")))))))js");
    const auto* const z = interactive.graph();

    // The part of the rule that specifies the condition, along with the
    // object of the rule's one consequence, whose predicate is `predicate`.
    const auto rule_parts = [z](const std::string& predicate)
    {
        for (const Node rule : z->get_left(z->core.Causes))
        {
            if (z->predicate_of(rule) != z->core.Causes) continue;
            adjacency_set consequences;
            const Node    condition = z->parse_fact(rule, consequences);
            if (consequences.size() != 1) continue;
            const Node consequence = *consequences.begin();
            if (z->predicate_of(consequence) != z->get_node(predicate)) continue;
            adjacency_set objects;
            z->parse_fact(consequence, objects);
            if (objects.size() == 1) return std::pair<Node, Node>{condition, *objects.begin()};
        }
        return std::pair<Node, Node>{0, 0};
    };

    const auto [macro_conditions, macro_collection] = rule_parts("likes");
    REQUIRE(macro_collection != 0);
    CHECK(Network::is_written_template(macro_collection));
    CHECK(z->is_condition_set(macro_conditions));
    CHECK_FALSE(Network::is_recipe(macro_conditions));

    const Node before = rule_parts("loves").second;
    REQUIRE(before != 0);
    CHECK_FALSE(Network::is_recipe(before));
    CHECK_FALSE(Zelph::is_hash(before));

    const Node plain = rule_parts("has").second;
    REQUIRE(plain != 0);
    CHECK_FALSE(Network::is_recipe(plain));
    CHECK_FALSE(Zelph::is_hash(plain));

    const Node by_hand = rule_parts("owns").second;
    REQUIRE(by_hand != 0);
    CHECK(Network::is_written_template(by_hand));
}

TEST_CASE("recipe_collection: the id finds the collection, whatever was written into it")
{
    // A firing that builds a term previously built must locate it solely by
    // its identifier. Comparing members instead would build the collection
    // again immediately upon any other rule writing to it, causing a run to
    // never end. The membership facts it writes are reported, enabling a
    // caller that builds a rule's parts to mark them with the rest.
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node in = z.core.PartOf;
    const Node id = Network::recipe_id(z.node("template"), Network::recipe_key({}), false);

    bool              created = false;
    std::vector<Node> asserted;
    CHECK(z.recipe_collection(id, {a, b}, created, &asserted) == id);
    CHECK(created);
    REQUIRE(z.exists(id));
    REQUIRE(z.check_fact(a, in, {id}).is_known());
    REQUIRE(z.check_fact(b, in, {id}).is_known());
    std::sort(asserted.begin(), asserted.end());
    std::vector<Node> expected{z.check_fact(a, in, {id}).relation(), z.check_fact(b, in, {id}).relation()};
    std::sort(expected.begin(), expected.end());
    CHECK(asserted == expected);
    CHECK_FALSE(z.is_set_constant(id));

    SUBCASE("built again, it is found and nothing is written")
    {
        const Node before = z.count();
        asserted.clear();
        CHECK(z.recipe_collection(id, {a, b}, created, &asserted) == id);
        CHECK_FALSE(created);
        CHECK(asserted.empty());
        CHECK(z.count() == before);
    }

    SUBCASE("a member another statement wrote into it stays")
    {
        z.fact(c, in, {id});
        const Node before = z.count();
        asserted.clear();
        CHECK(z.recipe_collection(id, {a, b}, created, nullptr) == id);
        CHECK_FALSE(created);
        CHECK(z.count() == before);
        CHECK(z.check_fact(c, in, {id}).is_known());
    }

    SUBCASE("two recipes that collide share one node, and the membership the second one wrote is reported")
    {
        // Reuse compares no members, so a recipe whose id collides with
        // another's is not differentiated from it: its members are
        // incorporated into the node the first one built. The membership
        // facts it writes are reported, enabling a construction to mark
        // them with the rest of the rule's parts rather than leaving them
        // to read as claims.
        asserted.clear();
        CHECK(z.recipe_collection(id, {a, b, c}, created, &asserted) == id);
        CHECK_FALSE(created);
        REQUIRE(z.check_fact(c, in, {id}).is_known());
        CHECK(asserted == std::vector<Node>{z.check_fact(c, in, {id}).relation()});
    }
}

TEST_CASE("recipe_collection: a variable's membership removed on its own is asserted again and reported")
{
    // A generated rule's own collection holds variables, and `.remove` can
    // take one such membership away by its id. This action leaves the
    // collection standing: a variable is not an element, so the removal
    // does not take the container with it, unlike when a ground member is
    // removed. The next construction under the same recipe finds the node,
    // asserts the membership again, and must report it, ensuring it is
    // marked with the rule's other parts rather than appearing as a claim
    // within the rule's own collection.
    //
    // A construction writes its rule's own collection during the act of
    // writing the rule, within the template scope, which the scope opened
    // here represents: beyond that boundary, fact() rejects a membership in
    // a collection of a rule's text.
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node x  = z.var();
    const Node in = z.core.PartOf;
    const Node id = Network::recipe_id(z.node("template"), Network::recipe_key({}), true);

    const Zelph::TemplateScope scope(z);
    bool                       created = false;
    z.recipe_collection(id, {a, x}, created, nullptr);
    REQUIRE(created);
    REQUIRE(z.check_fact(x, in, {id}).is_known());
    z.remove_node(z.check_fact(x, in, {id}).relation());
    REQUIRE(z.exists(id));
    REQUIRE_FALSE(z.check_fact(x, in, {id}).is_known());

    std::vector<Node> asserted;
    CHECK(z.recipe_collection(id, {a, x}, created, &asserted) == id);
    CHECK_FALSE(created);
    REQUIRE(z.check_fact(x, in, {id}).is_known());
    CHECK(asserted == std::vector<Node>{z.check_fact(x, in, {id}).relation()});
}

TEST_CASE("recipe_collection: dropping the cluster it was built in takes it back")
{
    // A rule is written within a scratch cluster, which is dropped if the
    // rule is found to already exist, and `.cluster-drop` takes back what
    // a user's cluster had recorded: a collection built there must go with
    // the cluster, including membership facts, and leave what existed
    // before.
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node in = z.core.PartOf;
    const Node id = Network::recipe_id(z.node("template"), Network::recipe_key({}), true);

    // The collection of a construction, written within the scope where a
    // rule is written, as a construction writes it.
    z.set_active_cluster("built");
    bool created = false;
    {
        const Zelph::TemplateScope scope(z);
        z.recipe_collection(id, {a}, created, nullptr);
    }
    z.deactivate_cluster();
    REQUIRE(created);
    REQUIRE(z.exists(id));

    CHECK(z.drop_cluster("built") > 0);
    CHECK_FALSE(z.exists(id));
    CHECK_FALSE(z.check_fact(a, in, {id}).is_known());
    CHECK(z.exists(a));
}

TEST_CASE("kind_unknowable: the one test of whether a literal can be a set constant")
{
    // `{...}` is identified by its members, which it cannot be if any of them
    // is a variable or holds one: set() thus creates a collection. A member
    // that is a COLLECTION holding a variable leaves the kind known -- a
    // collection is not a hash, so its members are not part of the fact
    // closure of anything that holds it -- and `{@{Y}}` is a set constant
    // around that single collection.
    Zelph      z(null_handler());
    const Node a       = z.node("a");
    const Node b       = z.node("b");
    const Node p       = z.node("p");
    const Node y       = z.var();
    const Node pattern = z.fact(y, p, {a});
    const Node ground  = z.fact(a, p, {b});
    const Node inner   = z.collection({y});

    CHECK_FALSE(z.kind_unknowable({a, b}));
    CHECK(z.kind_unknowable({a, y}));
    CHECK(z.kind_unknowable({a, pattern}));
    CHECK_FALSE(z.kind_unknowable({a, ground}));
    CHECK_FALSE(z.kind_unknowable({inner}));

    // The same test is read by
    // set().
    CHECK(z.is_set_constant(z.set({a, b})));
    CHECK_FALSE(Zelph::is_hash(z.set({a, y})));
    CHECK_FALSE(Zelph::is_hash(z.set({a, pattern})));
    CHECK(z.is_set_constant(z.set({a, ground})));
    CHECK(z.is_set_constant(z.set({inner})));
}

TEST_CASE("is_named_any: a name in any language, or a core spelling")
{
    // A named node is one shared object, regardless of the language
    // currently in use during the session: altering the language must
    // not alter the identity of a node, hence a name expressed in a
    // different language still counts, just as the spelling of a core node
    // does, which lives in its own table and not within the name maps.
    Zelph      z(null_handler());
    const Node a    = z.node("a");
    const Node b    = z.node("b");
    const Node p    = z.node("p");
    const Node anon = z.collection({a});
    const Node fact = z.fact(a, p, {b});

    CHECK(z.is_named_any(a));
    CHECK_FALSE(z.is_named_any(anon));
    CHECK_FALSE(z.is_named_any(fact));

    z.set_name(anon, "Behälter", "de", false);
    CHECK_FALSE(z.has_name(anon, z.lang()));
    CHECK(z.is_named_any(anon));

    // As in a session, the core spellings are
    // registered.
    CHECK_FALSE(z.is_named_any(z.core.Conjunction));
    z.register_core_node(z.core.Conjunction, "conjunction");
    CHECK(z.is_named_any(z.core.Conjunction));
}

TEST_CASE("fact: a membership in a collection of a rule's text is written only while a rule is written")
{
    // Once a rule is written, its text becomes immutable. A membership
    // written into one of its collections or into a rule's condition set
    // from another source -- a program holding the node, the C ABI -- would
    // alter what the rule says: the rule would print the member, its
    // fingerprint and the variables contained within its text would become
    // outdated, and the rule typed again would be a second rule. Such a
    // write is rejected, identifies the collection involved, and leaves the
    // graph in its original state. While a rule is being written -- the
    // template scope, which the parser, zelph/rule's argument forms,
    // zelph/rule-text, and a construction open -- the same write is how the
    // rule's text comes into being.
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node p  = z.node("p");
    const Node in = z.core.PartOf;
    const Node x  = z.var();

    Node own = 0;
    {
        const Zelph::TemplateScope scope(z);
        own = z.collection({a});
    }
    REQUIRE(z.is_rule_template(own));
    const Node conditions = z.conjunction_collection({z.fact(x, p, {a})});
    z.fact(conditions, z.core.IsA, {z.core.Conjunction});
    REQUIRE_FALSE(z.is_rule_template(conditions));

    const std::map<Node, Node> held{{own, a}, {conditions, z.fact(x, p, {a})}};
    for (const Node container : {own, conditions})
    {
        CAPTURE(container);
        const Node before = z.count();
        CHECK_THROWS_WITH_AS(z.fact(b, in, {container}), doctest::Contains("fixed once the rule is written"), std::runtime_error);
        CHECK_THROWS_AS(z.fact(x, in, {container}), std::runtime_error);
        // A member the collection already holds, stated again, contributes
        // no node, yet a statement constitutes a claim, and via the script
        // and the C interfaces it turned the rule's literal member into
        // data. It is refused just as every other write is.
        CHECK_THROWS_WITH_AS(z.fact(held.at(container), in, {container}), doctest::Contains("fixed once the rule is written"), std::runtime_error);
        CHECK(z.count() == before);
        CHECK_FALSE(z.check_fact(b, in, {container}).is_known());
        CHECK_FALSE(z.check_fact(x, in, {container}).is_known());
        CHECK(z.check_fact(held.at(container), in, {container}).is_known());
    }
    CHECK_THROWS_WITH(z.fact(b, in, {own}), doctest::Contains("a rule's own text"));
    CHECK_THROWS_WITH(z.fact(b, in, {conditions}), doctest::Contains("the condition set of a rule"));

    // A data collection takes the membership, and within the scope, the
    // rule's own collection also does.
    const Node data = z.collection({a});
    z.fact(b, in, {data});
    CHECK(z.check_fact(b, in, {data}).is_known());
    {
        const Zelph::TemplateScope scope(z);
        z.fact(b, in, {own});
    }
    CHECK(z.check_fact(b, in, {own}).is_known());
}

TEST_CASE("is_rule_template: the id class alone decides")
{
    // Whether a collection qualifies as a rule's own is determined at the
    // moment of its creation: nothing written later -- a statement naming
    // it, a name, a membership -- can transform a template into a value or
    // a value into a template. A membership written into a template from
    // outside a rule is refused, so it cannot try.
    Zelph      z(null_handler());
    const Node a     = z.node("a");
    const Node p     = z.node("p");
    const Node in    = z.core.PartOf;
    const Node data  = z.collection({a});
    const Node set   = z.set({a});
    const Node key   = Network::recipe_key({{0xFFFFFFFFFFFFFFF0ull, a}});
    const Node built = Network::recipe_id(data, key, true);

    CHECK(z.is_rule_template(0x3000000000000007ull)); // a written template
    CHECK(z.is_rule_template(built));                 // a construction's recipe
    CHECK_FALSE(z.is_rule_template(Network::recipe_id(data, key, false)));
    CHECK_FALSE(z.is_rule_template(Zelph::bucket_term_id(data)));
    CHECK_FALSE(z.is_rule_template(data));
    CHECK_FALSE(z.is_rule_template(set));
    CHECK_FALSE(z.is_rule_template(a));
    CHECK_FALSE(z.is_rule_template(z.var()));

    bool created = false;
    {
        const Zelph::TemplateScope scope(z); // as a construction writes it
        z.recipe_collection(built, {a}, created, nullptr);
    }
    z.fact(a, p, {built});
    CHECK_THROWS_AS(z.fact(z.node("b"), in, {built}), std::runtime_error);
    z.fact(z.node("b"), in, {data});
    z.set_name(built, "named", "en", false);
    z.fact(a, p, {data});
    CHECK(z.is_rule_template(built));
    CHECK_FALSE(z.is_rule_template(data));
}
