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

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <tuple>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// A rule as the CONSEQUENCE of another rule.
//
// Reasoning ABOUT statements is what zelph is for, and the sharpest form of
// it is a rule that produces a rule: "whatever is transitive chains", "while
// this switch is on, that rule holds". Nothing else in the engine can say
// that -- a query language cannot, and a rule engine whose consequences are
// facts cannot either.
//
// A rule is not a fact with a different predicate. Its subject is either one
// condition pattern or a conjunction SET node, and that set node is created
// rather than hash-consed; its members hang off it as separate PartOf facts;
// and the tags that make the engine read it as a conjunction, or a member as
// a negation, are facts of their own. Instantiating the top-level triple --
// which is all a fact needs -- therefore produced a rule whose conditions
// still carried the unbound pattern variables while its conclusion had been
// filled with freshly created nodes: inert junk. These tests pin the two
// halves that make the difference, the structure and the QUANTIFICATION (the
// inner rule's variables stay variables), plus the three properties without
// which the feature is unusable at all: it terminates, it survives a save,
// and .explain can reconstruct through it.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // How many rules ".list-rules" just listed.
    std::size_t listed_rules(const zelph::io::OutputCollector& collector)
    {
        std::size_t n = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).find("=>") != std::string::npos) ++n;
        return n;
    }

    // The "Nodes: N" line of .stat.
    std::string node_count(const zelph::io::OutputCollector& collector)
    {
        for (const auto& e : collector.events())
        {
            const std::string t = normalize(e.text);
            if (t.rfind("Nodes:", 0) == 0) return t;
        }
        return {};
    }

    // Runs a second time and checks that the run introduces no additional
    // node: what the first run constructed is retrieved once more rather
    // than being built a second time.
    void rerun_adds_no_node(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(".stat");
        const std::string before = node_count(collector);
        REQUIRE_FALSE(before.empty());

        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == before);
    }

    // Enters `trigger`, causing a generator to fire once again using a
    // binding previously employed, and checks that no new rule is
    // introduced: the firing finds every rule it writes, with the same
    // collections it was written with the first time.
    void trigger_adds_no_rule(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& trigger)
    {
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t before = listed_rules(collector);

        interactive.process(trigger);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == before);
    }

    // Enters `rule` and subsequently `trigger`, which establishes no binding
    // within the rule that `rule` writes. The construction of that rule
    // lands on the node `rule` refers to, and `build_rule` claims an
    // alpha-renamed copy instead, or the duplicate it had claimed on an
    // earlier pass; when the rule holds a collection, the construction
    // builds it as one of its own, and the rule it writes becomes a node in
    // its own right.
    // In either case, the next pass must identify that rule: otherwise each
    // pass writes an additional one and the run never concludes. Auto-run
    // remains disabled during the counting of passes, ensuring that such an
    // engine fails the REQUIRE instead. Auto-run resumes afterwards.
    void claims_once(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& rule, const std::string& trigger)
    {
        interactive.process(".auto-run"); // a toggle: off
        interactive.process(rule);
        interactive.process(trigger);
        for (int pass = 0; pass < 3; ++pass)
            interactive.process(".run-once");

        // The rule, and the single copy it
        // claimed.
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 2);

        interactive.process(".auto-run"); // on again
    }

    // The identifier that `.node` lists for the neighbour displayed as
    // `label`, or an empty string: the method by which a user finds a node
    // lacking a name.
    std::string listed_id(const zelph::io::OutputCollector& collector, const std::string& label)
    {
        const std::string prefix = "- " + label + " (ID ";
        for (const auto& e : collector.events())
        {
            const std::size_t at = e.text.find(prefix);
            if (at == std::string::npos) continue;
            const std::size_t from = at + prefix.size();
            return e.text.substr(from, e.text.find(')', from) - from);
        }
        return {};
    }

    // `predicates` in the sequence established by the lines in `collector`
    // first naming a rule over each, `(X <predicate> @{...})`: the order in
    // which a firing wrote the rules.
    std::vector<std::string> announced_order(const zelph::io::OutputCollector& collector, std::vector<std::string> predicates)
    {
        const auto& events    = collector.events();
        const auto  announced = [&](const std::string& predicate)
        {
            return std::find_if(events.begin(), events.end(), [&](const auto& e)
                                { return e.text.find("(X " + predicate + " @{") != std::string::npos; })
                 - events.begin();
        };
        for (const auto& predicate : predicates)
            REQUIRE(announced(predicate) < static_cast<std::ptrdiff_t>(events.size()));
        std::ranges::sort(predicates, {}, announced);
        return predicates;
    }

    // Removes the node that `.node <anchor>` lists as `label`, identified
    // via the id that `.node` lists, just as a user finds a node lacking
    // a name.
    void remove_listed(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& anchor, const std::string& label)
    {
        collector.clear();
        interactive.process(".node " + anchor);
        const std::string id = listed_id(collector, label);
        REQUIRE_FALSE(id.empty());
        interactive.process(".remove " + id);
    }

    // Removes the `@{(Z q k)}` collection associated with the fact that
    // `.node <anchor>` lists as `fact`.
    void remove_collection_in(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& anchor, const std::string& fact)
    {
        collector.clear();
        interactive.process(".node " + anchor);
        const std::string id = listed_id(collector, fact);
        REQUIRE_FALSE(id.empty());
        remove_listed(collector, interactive, id, "@{(Z q k)}");
    }

    // Eliminates the collection associated with the rule that outputs
    // `X <predicate> @{(Z q k)}`, identified via the rule's consequence. The
    // fact `b <predicate> @{(Z q k)}` a firing derived names the term of that
    // collection, not the collection itself.
    void remove_collection_of(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& predicate)
    {
        remove_collection_in(collector, interactive, predicate, "X " + predicate + " @{(Z q k)}");
    }

    // The `key` counter within the construction section of `.prof`, which
    // counts while `.log -1` is active: `built` constructions and
    // `remembered` claims.
    std::uint64_t construction_counter(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& key)
    {
        collector.clear();
        interactive.process(".prof");
        std::string prof;
        for (const auto& event : collector.events())
            prof += event.text + "\n";
        const std::size_t section = prof.find("construction:");
        REQUIRE(section != std::string::npos);
        const std::size_t at = prof.find(key + "=", section);
        REQUIRE(at != std::string::npos);
        return std::stoull(prof.substr(at + key.size() + 1));
    }
}

TEST_CASE("derived rules: a transitivity generator chains the relation it names")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.process("before is transitive");
        interactive.process("a before b");
        interactive.process("b before c");
        interactive.process("c before d");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A before D");
        CHECK(answers_contain(collector, "a before c"));
        CHECK(answers_contain(collector, "b before d"));
        CHECK(answers_contain(collector, "a before d"));

        // The generator and exactly one derived rule.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);
        // "??" is how an unnamed node prints. The derived rule used to be
        // built out of them: its conclusion's variables had been replaced by
        // freshly created nodes while its conditions kept the pattern
        // variables, so it matched nothing and said nothing.
        CHECK_FALSE(any_output_contains(collector, "??")); });
}

TEST_CASE("derived rules: the facts may be older than the rule that derives the rule")
{
    // A derived rule has to see the graph it was born into, not just what
    // arrives after it. That is the classic pass the fixpoint loop repeats
    // once the rule set has grown.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a before b");
        interactive.process("b before c");
        interactive.process("c before d");
        interactive.process("before is transitive");
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A before D");
        CHECK(answers_contain(collector, "a before d")); });
}

TEST_CASE("derived rules: deriving the same rule again changes nothing")
{
    // The conjunction set node is CREATED, not hash-consed, so nothing
    // collapses two copies of a derived rule by itself. Without an exact
    // duplicate check every run would build another set node, another rule
    // and another reason to run again -- the fixpoint would never arrive.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.process("before is transitive");
        interactive.process("a before b");
        interactive.process("b before c");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".stat");
        const std::string before = node_count(collector);
        REQUIRE_FALSE(before.empty());

        interactive.run(true, false, false);
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == before);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

TEST_CASE("derived rules: the rules a generator writes for two relations do not blend")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.process("before is transitive");
        interactive.process("smaller is transitive");
        interactive.process("a before b");
        interactive.process("b before c");
        interactive.process("p smaller q");
        interactive.process("q smaller r");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3);

        collector.clear();
        interactive.process("A before B");
        CHECK(answers_contain(collector, "a before c"));

        collector.clear();
        interactive.process("A smaller B");
        CHECK(answers_contain(collector, "p smaller r"));
        // The two derived rules must not blend: `before` and `smaller` share
        // no facts, so a rule quantified over the wrong one would show up
        // here as a cross-relation conclusion.
        CHECK_FALSE(answers_contain(collector, "a smaller c")); });
}

TEST_CASE("derived rules: a rule under a switch stays inert until the switch is on")
{
    // The shape a user reaches for first, and the one that makes the
    // difference between mentioning a rule and asserting it visible: the
    // inner rule is written out in full, so it is IN the graph from the
    // start -- but as the object of the outer rule, i.e. mentioned, not
    // claimed. Only the outer rule firing claims it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(K is on) => ((X p Y) => (X q Y))");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A q B");
        REQUIRE_FALSE(answers_contain(collector, "a q b"));

        interactive.process("k is on");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A q B");
        CHECK(answers_contain(collector, "a q b")); });
}

TEST_CASE("derived rules: a switched multi-condition rule works the same way")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(K is on) => ((X p Y, Y p Z) => (X q Z))");
        interactive.process("a p b");
        interactive.process("b p c");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A q B");
        REQUIRE_FALSE(answers_contain(collector, "a q c"));

        interactive.process("k is on");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A q B");
        CHECK(answers_contain(collector, "a q c"));

        // Switching it on twice is not two rules.
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

TEST_CASE("derived rules: a negated condition survives the derivation")
{
    // The negation tag is a fact ABOUT the condition pattern, not a part of
    // it, so instantiation cannot carry it along -- it has to be restated.
    // A derived rule that lost it would silently mean the opposite.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R needs check) => ((X R Y, ¬(Y bad yes)) => (X ok Y))");
        interactive.process("p needs check");
        interactive.process("c bad yes");
        interactive.process("a p b");
        interactive.process("a p c");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "¬"));

        collector.clear();
        interactive.process("A ok B");
        CHECK(answers_contain(collector, "a ok b"));
        CHECK_FALSE(answers_contain(collector, "a ok c")); });
}

TEST_CASE("derived rules: a negated condition of a rule nested in a generated rule takes the outer binding")
{
    // One level deeper than the preceding case. The generated rule's
    // CONSEQUENCE is itself a rule containing a conjunction, and that
    // conjunction set remained unaltered through substitution: the inner
    // rule was written as
    //
    //     (¬(Y blocks H), (Y q a)) => (Y works a)
    //
    // with H, the generator's variable, left free. `¬(Y blocks H)`
    // subsequently inquires whether b blocks ANYTHING, thus `b blocks m`
    // suppressed `b works a`, although only `b blocks k` should.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The blockers appear first: in check mode, a negation that
        // subsequently comes to hold after a fact has been derived
        // under it is rejected.
        process_lines(interactive, R"(
b blocks m
c blocks k
(G go H) => ((X p H) => ((Y q X, ¬(Y blocks H)) => (Y works X)))
now go k
a p k
b q a
c q a
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S works O");
        CHECK(answers_contain(collector, "b works a"));
        CHECK_FALSE(answers_contain(collector, "c works a")); });
}

TEST_CASE("derived rules: a single negated condition of a rule nested in a generated rule stays negated")
{
    // The tag that says a condition is negated constitutes a fact in its own
    // right, and only the construction of the OUTERMOST derived rule restated
    // it. One level beneath, `¬(X blocks H)` emerged as `(X blocks k)`: a
    // positive condition, meaning the rule said the opposite of what was
    // written and derived `k works yes` BECAUSE z blocks k.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
z blocks k
(G go H) => ((H is on) => (¬(X blocks H) => (H works yes)))
now go k
now go j
k is on
j is on
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S works O");
        CHECK(answers_contain(collector, "j works yes"));
        CHECK_FALSE(answers_contain(collector, "k works yes"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "¬(X blocks k)")); });
}

TEST_CASE("derived rules: a derived rule may derive a rule in turn")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(K is on) => ((P entails Q) => ((X P Y) => (X Q Y)))");
        interactive.process("k is on");
        interactive.process("parent entails ancestor");
        interactive.process("a parent b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A ancestor B");
        CHECK(answers_contain(collector, "a ancestor b"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3); });
}

TEST_CASE("derived rules: .explain reconstructs a proof through the derived rule")
{
    // The whole tree, not merely the facts contained within it: the
    // generator's template functions as a rule that the generator solely
    // mentions, and while the search used it as a rule, the same facts
    // emerged within a tree that announced a second justification, while an
    // unrelated fact lost its [axiom] because the template's variable
    // predicate was unified with every fact. The premises of the derived rule
    // stay "asserted; no derivation found": the rule currently in force,
    // (X before Y, Y before Z) => (X before Z), matches every `before`
    // fact, exactly as the identical rule manually entered does.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.process("before is transitive");
        interactive.process("a before b");
        interactive.process("b before c");
        interactive.process("x after y");
        interactive.run(true, false, false);

        // The premises follow the memberships of the derived
        // rule's condition set, maintaining the sequence dictated by
        // node ids, thus they are compared as a set.
        collector.clear();
        interactive.process(".explain (a before c)");
        const std::string        first = "   ├─ ";
        const std::string        last  = "   └─ ";
        std::vector<std::string> lines;
        std::istringstream       tree(last_out_text(collector));
        for (std::string line; std::getline(tree, line);)
            lines.push_back(line);
        REQUIRE(lines.size() == 3);
        CHECK(lines[0] == "a before c");
        REQUIRE(lines[1].starts_with(first));
        REQUIRE(lines[2].starts_with(last));
        std::vector<std::string> premises{lines[1].substr(first.size()), lines[2].substr(last.size())};
        std::sort(premises.begin(), premises.end());
        CHECK(premises == std::vector<std::string>{"a before b  [asserted; no derivation found]", "b before c  [asserted; no derivation found]"});

        collector.clear();
        interactive.process(".explain (x after y) 0");
        CHECK(last_out_text(collector) == "x after y  [axiom]\n"); });
}

// A derived rule possesses a derivation like any derived fact: the
// instantiation of the generator responsible for its construction. .explain
// answered [axiom] for it, since unification does not match a rule-shaped
// consequence against a rule; the search now asks the generator's own
// construction to determine which rule each of its solutions represents
// (Reasoning::existing_rule). A fact derived THROUGH the generated rule
// continues to display solely its fact premises, as previously illustrated:
// the rule's own justification is what the explanation of the rule shows.
TEST_CASE("derived rules: a generated rule is explained by its generator")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(C subclassof D) => ((X isa C) => (X isa D))
person subclassof agent
(R is transitive) => ((X R Y, Y R Z) => (X R Z))
before is transitive
(K is on) => ((P entails Q) => ((X P Y) => (X Q Y)))
k is on
parent entails ancestor
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain ((X isa person) => (X isa agent)) 0");
        CHECK(last_out_text(collector) == R"((X isa person) => (X isa agent)
   └─ person subclassof agent  [axiom]
)");

        // Two conditions: the generator finds the conjunction set it built.
        // The order of the conditions adheres to the node identifiers, thus
        // only the premise line is compared exactly.
        collector.clear();
        interactive.process(".explain ((X before Y, Y before Z) => (X before Z)) 0");
        const std::string transitive = last_out_text(collector);
        CHECK(transitive.find("=> (X before Z)\n   └─ before is transitive  [axiom]\n") != std::string::npos);
        CHECK(transitive.find("[axiom]") == transitive.rfind("[axiom]"));

        // A rule derived by another rule, which itself was derived in
        // turn: each level is justified by the declaration that activated
        // it.
        collector.clear();
        interactive.process(".explain ((X parent Y) => (X ancestor Y)) 0");
        CHECK(last_out_text(collector) == R"((X parent Y) => (X ancestor Y)
   └─ parent entails ancestor  [axiom]
)");
        collector.clear();
        interactive.process(".explain ((P entails Q) => ((X P Y) => (X Q Y))) 0");
        CHECK(last_out_text(collector) == R"((P entails Q) => ((X P Y) => (X Q Y))
   └─ k is on  [axiom]
)"); });
}

// The generator rebuilds a conjunction nested within the rule it writes,
// along with a container that keeps a variable from that rule, both serving
// as collections of the rule it writes. The search predicts each one as the
// construction builds it -- the conjunction is identified through its
// members, the container is the collection of its recipe -- and if either is
// treated as anything other than what it is, the rule would appear missing
// from the graph, and the generated rule would lose its derivation.
TEST_CASE("derived rules: a generated rule with a rebuilt collection is explained by its generator")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((H is on) => ((H partof I, I q H) => (H is green)))
(G go H) => ((X p H) => (X likes {(H is green) (X q H)}))
now go k
)");
        interactive.run(true, false, false);

        // The sequence in which a collection's members are arranged is
        // determined by the node ids, meaning that only the premise line is
        // compared exactly.
        collector.clear();
        interactive.process(".explain ((k is on) => ((k partof I, I q k) => (k is green))) 0");
        const std::string nested = last_out_text(collector);
        CHECK(nested.find("=> (k is green))\n   └─ now go k  [axiom]\n") != std::string::npos);
        CHECK(nested.find("[axiom]") == nested.rfind("[axiom]"));

        collector.clear();
        interactive.process(".explain ((X p k) => (X likes {(k is green) (X q k)})) 0");
        const std::string container = last_out_text(collector);
        CHECK(container.find(")})\n   └─ now go k  [axiom]\n") != std::string::npos);
        CHECK(container.find("[axiom]") == container.rfind("[axiom]")); });
}

// A part of the template that holds a collection of the rule's text: either
// such a collection itself, or a fact concerning one. The construction
// regenerates both, regardless of whether a variable is present within them,
// and when considered as a ground pattern, such a part would have to be the
// exact node the rule holds: the generator would be disregarded as one whose
// template is incapable of forming this rule, and the rule would be
// interpreted as an axiom.
//
// Here, a container of the template that the substitution makes ground: the
// generator holds `{(H is green) (k q k)}` as a collection, because it holds
// H, and the rule it produces holds the set constant
// `{(k is green) (k q k)}`.
TEST_CASE("derived rules: a generated rule whose collection became a set constant is explained by its generator")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes {(H is green) (k q k)}))
now go k
)");
        interactive.run(true, false, false);

        // The order of a set's members is determined by the node ids,
        // meaning that only the premise line is compared exactly.
        collector.clear();
        interactive.process(".explain ((X p k) => (X likes {(k is green) (k q k)})) 0");
        const std::string container = last_out_text(collector);
        CHECK(container.find(")})\n   └─ now go k  [axiom]\n") != std::string::npos);
        CHECK(container.find("[axiom]") == container.rfind("[axiom]")); });
}

TEST_CASE("derived rules: a generated rule whose consequence holds variables only in collections is explained by its generator")
{
    // `(@{(Y q H)} likes @{(Y q H)})` holds H solely within its two
    // collections, `(k loves @{(H q Z)})` exclusively within its single
    // one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (@{(Y q H)} likes @{(Y q H)}))
(G go H) => ((X s H) => (k loves @{(H q Z)}))
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain ((X p k) => (@{(Y q k)} likes @{(Y q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (@{(Y q k)} likes @{(Y q k)})
   └─ now go k  [axiom]
)");

        collector.clear();
        interactive.process(".explain ((X s k) => (k loves @{(k q Z)})) 0");
        CHECK(last_out_text(collector) == R"((X s k) => (k loves @{(k q Z)})
   └─ now go k  [axiom]
)"); });
}

TEST_CASE("derived rules: a generated rule whose condition holds a variable only in a set is explained by its generator")
{
    // The condition `(k t {H a})` holds H solely within its container,
    // which the substitution makes the set constant `{m a}`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((k t {H a}) => (k u n))
now go m
)");
        interactive.run(true, false, false);

        // The sequence in which the set's members are arranged adheres
        // to the node identifiers, meaning that only the premise line is
        // compared exactly.
        collector.clear();
        interactive.process(".explain ((k t {m a}) => (k u n)) 0");
        const std::string condition = last_out_text(collector);
        CHECK(condition.find("=> (k u n)\n   └─ now go m  [axiom]\n") != std::string::npos);
        CHECK(condition.find("[axiom]") == condition.rfind("[axiom]")); });
}

TEST_CASE("derived rules: a generator a switch wrote over collections is explained by the switch")
{
    // The innermost consequence holds its sole variable, H, within
    // `@{(Y q H)}`, inside the generator that the switch writes as in the
    // template from which the switch writes it. Every level is justified by
    // the declaration that activated it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(K is on) => ((G go H) => ((X p H) => (@{(Y q H)} likes @{(Y q k)})))
sw is on
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain ((X p k) => (@{(Y q k)} likes @{(Y q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (@{(Y q k)} likes @{(Y q k)})
   └─ now go k  [axiom]
)");

        collector.clear();
        interactive.process(".explain ((G go H) => ((X p H) => (@{(Y q H)} likes @{(Y q k)}))) 0");
        CHECK(last_out_text(collector) == R"((G go H) => ((X p H) => (@{(Y q H)} likes @{(Y q k)}))
   └─ sw is on  [axiom]
)"); });
}

// Two collections sharing identical members, each rebuilt for the generated
// rule as a standalone collection. The search must locate them just as the
// construction does, one for each: if the first collection is found twice,
// it would seek a rule with a single object, which the graph does not hold,
// and the generated rule would lose its derivation.
TEST_CASE("derived rules: a generated rule with two equal collections is explained by its generator")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Y q H)} @{(Y q H)}))
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain ((X p k) => (X likes @{(Y q k)} @{(Y q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (X likes @{(Y q k)} @{(Y q k)})
   └─ now go k  [axiom]
)"); });
}

// A firing substitutes a collection's term for that collection within the
// rule's text, so the fact it derives identifies the term, not the rule's
// own collection. Matching the rule's consequence to that fact via identity
// finds no rule that leads to it, and the fact is interpreted as an axiom. A
// set constant that has been rebuilt goes the same way.
TEST_CASE("derived rules: a fact over a container an ordinary rule rebuilt is explained by that rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{(Z q Y)})
(X p Y) => (X hates {Y a})
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("b likes S");
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector) == R"(b likes @{(Z q k)}
   └─ b p k  [axiom]
)");

        // The order of the members adheres to the node ids,
        // meaning only the premise line is compared exactly.
        collector.clear();
        interactive.process("b hates S");
        REQUIRE(collect_answers(collector).size() == 1);
        collector.clear();
        interactive.process(".explain");
        const std::string set = last_out_text(collector);
        CHECK(set.find("}\n   └─ b p k  [axiom]\n") != std::string::npos);
        CHECK(set.find("[axiom]") == set.rfind("[axiom]")); });
}

// After its premise is pruned, a fact that a rule's consequence matches is
// reported as asserted with no derivation found, rather than as an axiom: a
// rule concludes facts of its specific shape, and the search finds no
// instantiation that holds. The rule's own collection represents the terms
// a firing builds in that position, and exclusively those: with nothing to
// substitute, a firing still builds a term for it, whereas a collection
// written as data is none, so a fact containing one in that position
// remains an axiom.
TEST_CASE("derived rules: a fact over a collection with nothing to substitute keeps its status once its premise is gone")
{
    const auto check = [](auto& collector, auto& interactive, const std::string& rules)
    {
        process_lines(interactive, rules);
        interactive.process("b p k");
        interactive.process(".prune-facts b p k");

        collector.clear();
        interactive.process("b likes S");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);

        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector) == answers.front() + "  [asserted; no derivation found]\n");

        interactive.process("c likes @{(z q m)}");
        collector.clear();
        interactive.process("c likes S");
        const auto other = collect_answers(collector);
        REQUIRE(other.size() == 1);
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector) == other.front() + "  [axiom]\n");
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("a typed rule")
        {
            check(collector, interactive, "(X p k) => (X likes @{(Z q k)})");
        }
        SUBCASE("a generated rule")
        {
            check(collector, interactive, "(G go H) => ((X p H) => (X likes @{(Z q H)}))\nnow go k");
        } });
}

// A collection that the firing replaces by a term represents any such term
// while the consequence is matched, because the specific term the firing
// builds is known only under the bindings of the entire rule. This kind of
// match constitutes a consequence that matches the fact, just as the
// identity match of any other part does, hence the status of a fact whose
// premise has vanished is the same. A fact featuring an atom in that
// position, or containing more objects than the consequence names, is not
// one that the rule concludes.
TEST_CASE("derived rules: a fact over a container a rule rebuilds keeps its status once its premise is gone")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{(Z q Y)})
b p k
.prune-facts b p k
)");

        collector.clear();
        interactive.process("b likes S");
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector) == "b likes @{(Z q k)}  [asserted; no derivation found]\n");

        interactive.process("b likes c");
        collector.clear();
        interactive.process(".explain (b likes c)");
        CHECK(last_out_text(collector) == "b likes c  [axiom]\n");

        interactive.process("b likes @{(z q m)} c");
        collector.clear();
        interactive.process("S likes O c");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector) == answers.front() + "  [axiom]\n"); });
}

// .explain keeps the rules in force across successive explanations as long as
// no change occurs in the source from which they were originally read. With
// each rule it keeps the collections of its consequences that stand for the
// terms a firing builds. When a member is removed from such a collection, the
// situation alters: absent `Y in @{Y}`, the expression
// `(X p Y) => (X likes @{Y})` names a collection without members. A firing
// keeps this collection, writes it into `b likes ??` exactly as it stands, and
// the fact is matched by identity. Had this collection been kept from an
// earlier explanation prior to the removal, it would still stand for any term,
// causing `b likes ??` to be explained differently than in a session where no
// prior explanation existed.
TEST_CASE("derived rules: a fact over a container that a removal left without a variable is explained as without an earlier explanation")
{
    const auto session = [](const bool explain_first)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "(X p Y) => (X likes @{Y})\nb p k\n");
        if (explain_first)
        {
            collector.clear();
            interactive.process(".explain (b likes {k}) 0");
            REQUIRE(last_out_text(collector) == "b likes {k}\n   └─ b p k  [axiom]\n");
        }

        remove_listed(collector, interactive, "Y", "Y in @{Y}");
        collector.clear();
        interactive.run(true, false, false);
        REQUIRE(any_output_contains(collector, "(b likes ??" // split: `??)` is a trigraph
                                               ") ⇐ (b p k)"));

        // The fact that the run just
        // derived.
        collector.clear();
        interactive.process(".explain");
        return last_out_text(collector);
    };
    CHECK(session(true) == session(false));
    CHECK(session(true) == "b likes ??\n   └─ b p k  [axiom]\n");
}

TEST_CASE("derived rules: a derived rule survives .save and .load")
{
    const auto file = fs::temp_directory_path() / "zelph_derived_rule_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.process("before is transitive");
        interactive.process("a before b");
        interactive.run(true, false, false);
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);

        // .load disables auto-run, so the new fact needs an explicit run.
        interactive.process("b before c");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A before B");
        CHECK(answers_contain(collector, "a before c"));
    }

    fs::remove(file);
}

TEST_CASE("derived rules: property axioms as data drive an RDFS-style closure")
{
    // The end-to-end case for the feature, and the rationale behind it: the
    // six property axioms typically used to describe an ontology --
    // transitive, symmetric, sub-property, sub-class, domain, range -- are
    // defined just ONCE as rule generators, and each declaration a modeller
    // enters thereafter is ordinary data that produces its own specialised
    // rule.
    //
    // Here is nothing that can be formulated as a query, and none of it works
    // without rules deriving rules: a generator fires on a DECLARATION and must
    // leave a rule behind, quantified over the data the declaration says
    // nothing about.
    //
    // The closure is countable by hand, which is what makes this a test:
    // exactly seven facts follow, and one of them (m isa agent) only through
    // a chain of three DERIVED rules -- sub-property, then domain, then
    // sub-class.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(P is transitive) => ((X P Y, Y P Z) => (X P Z))
(P is symmetric) => ((X P Y) => (Y P X))
(P subpropertyof Q) => ((X P Y) => (X Q Y))
(C subclassof D) => ((X isa C) => (X isa D))
(P domain C) => ((X P Y) => (X isa C))
(P range C) => ((X P Y) => (Y isa C))
partof is transitive
sibling is symmetric
mother subpropertyof parent
parent domain person
parent range person
person subclassof agent
a partof b
b partof c
x sibling y
m mother n
)");
        interactive.run(true, false, false);

        // Six generators along with a single derived rule for
        // each declaration.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 12);

        collector.clear();
        interactive.process("S partof O");
        CHECK(answers_contain(collector, "a partof c"));
        // partof was declared transitive, not symmetric.
        CHECK_FALSE(answers_contain(collector, "b partof a"));

        collector.clear();
        interactive.process("S sibling O");
        CHECK(answers_contain(collector, "y sibling x"));

        collector.clear();
        interactive.process("S parent O");
        CHECK(answers_contain(collector, "m parent n"));

        collector.clear();
        interactive.process("S isa O");
        CHECK(answers_contain(collector, "m isa person"));
        CHECK(answers_contain(collector, "n isa person"));
        CHECK(answers_contain(collector, "m isa agent"));
        CHECK(answers_contain(collector, "n isa agent"));
        // Nothing types the endpoints of `sibling` or `partof`.
        CHECK_FALSE(answers_contain(collector, "x isa person"));
        CHECK_FALSE(answers_contain(collector, "a isa person"));

        // The four-step chain, reconstructed: mother → parent → person →
        // agent, with each step through a rule that had itself been
        // derived. The entire tree is compared: while the search also used
        // the schemas' templates as rules, it printed the identical facts
        // in reverse sequence -- (m mother n) derived from (m parent n),
        // which it labelled as asserted -- under a second justification
        // that did not exist.
        collector.clear();
        interactive.process(".explain (m isa agent)");
        CHECK(last_out_text(collector) == R"(m isa agent
   └─ m isa person
      └─ m parent n
         └─ m mother n  [axiom]
)"); });
}

TEST_CASE("derived rules: a whole chain of derived rules settles in one run")
{
    // Every `chains` declaration produces a rule, and those rules feed each
    // other: `a p1 b` has to travel five of them. The fixpoint loop collects
    // its rule set once, so all of this depends on it noticing that the set
    // grew and collecting again -- and on stopping when it has not.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R chains S) => ((X R Y) => (X S Y))
p1 chains p2
p2 chains p3
p3 chains p4
p4 chains p5
a p1 b
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 5);

        collector.clear();
        interactive.process("A p5 B");
        CHECK(answers_contain(collector, "a p5 b"));

        collector.clear();
        interactive.process("A p3 B");
        CHECK(answers_contain(collector, "a p3 b")); });
}

TEST_CASE("derived rules: many matches make many rules, and duplicates make one")
{
    // The shape a rule generator has in practice: the generating rule matches
    // the network wherever it can, and each match fixes the variables of one
    // new rule. Two matches that fix them the SAME way must not make two
    // rules -- and nothing collapses them by itself, because the variables of
    // a rule are nodes of their own and a conjunction set is created rather
    // than hash-consed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("one condition")
        {
            interactive.process("(A knows B) => ((X p B) => (X q B))");
            interactive.process("tom knows red");
            interactive.process("sue knows red");  // same B -- same rule
            interactive.process("ann knows blue"); // different B -- another rule
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 3);
        }
        SUBCASE("several conditions, i.e. through the conjunction set")
        {
            interactive.process("(A knows B) => ((X p B, X r B) => (X q B))");
            interactive.process("tom knows red");
            interactive.process("sue knows red");
            interactive.process("jim knows red");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        } });
}

TEST_CASE("derived rules: one generator, one rule per declaration, all of them live")
{
    // Six declarations, six rules, and every one of them closes its own
    // three-element chain -- the generated rules must stay apart, which is
    // what "the variables are fixed by the match that made the rule" means.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        for (int i = 1; i <= 6; ++i)
        {
            const std::string r = "rel" + std::to_string(i);
            const std::string s = std::to_string(i);
            interactive.process(r + " is transitive");
            interactive.process("a" + s + " " + r + " b" + s);
            interactive.process("b" + s + " " + r + " c" + s);
        }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 7);

        for (int i = 1; i <= 6; ++i)
        {
            const std::string r = "rel" + std::to_string(i);
            const std::string s = std::to_string(i);
            collector.clear();
            interactive.process("S " + r + " O");
            CHECK(answers_contain(collector, "a" + s + " " + r + " c" + s));
            // and nothing crossed over into a neighbouring relation
            CHECK_FALSE(answers_contain(collector, "a1 " + r + " c2"));
        } });
}

TEST_CASE("derived rules: a generator may generate a generator")
{
    // Four levels: the outermost rule writes a rule that writes a rule that
    // writes the rule which finally fires on data. Parsing the nesting and
    // executing it are two different questions and both are asked here.
    //
    // Note what the second level is: "(k is on) => (…)" has a GROUND
    // condition, so this chain also depends on a ground condition being
    // allowed to match at all.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((H is on) => ((P entails Q) => ((X P Y) => (X Q Y))))");
        interactive.process("now go k");

        // The switch has not been activated: "k is on" serves as the
        // condition intrinsic to the generated rule, and the act of
        // writing the rule did not claim it.
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 2);

        interactive.process("k is on");
        interactive.process("parent entails ancestor");
        interactive.process("a parent b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A ancestor B");
        CHECK(answers_contain(collector, "a ancestor b"));

        // The generator, the two it generated, and the rule that fired.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 4); });
}

TEST_CASE("derived rules: a generated generator substitutes into its inner rule's conjunction")
{
    // The case above features a single condition at each level.
    // When two are present, the inner rule's conditions branch from a
    // conjunction SET node, and the substitution handed that node back
    // unaltered: `now go k` wrote
    //
    //     (k is on) => (((I q H), (H partof I)) => (k is green))
    //
    // with H free in the conditions, so `m partof n, n q m` derived
    // `k is green` -- this is an incorrect derivation, not one that is
    // absent. The typed rule
    // `(k is on) => ((k partof I, I q k) => (k is green))` derives
    // nothing from m and n.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((H is on) => ((H partof I, I q H) => (H is green)))");
        interactive.process("now go k");

        // The ground consequence resulting from the substitution is a
        // pattern, here as in the single-condition shape.
        collector.clear();
        interactive.process("A is green");
        REQUIRE_FALSE(answers_contain(collector, "k is green"));

        process_lines(interactive, R"(
k is on
m partof n
n q m
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A is green");
        CHECK_FALSE(answers_contain(collector, "k is green"));

        // H remains solely within the generator
        // itself.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(count_outputs_containing(collector, "H partof") == 1);

        process_lines(interactive, R"(
k partof x
x q k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A is green");
        CHECK(answers_contain(collector, "k is green")); });
}

TEST_CASE("derived rules: a switch over a switch renames the inner conjunction with its consequence")
{
    // `k is on` does not bind anything within the rule situated beneath the
    // outer one, hence constructing that rule lands on the node referenced
    // by the generator, and `build_rule` asserts ownership over it using
    // renamed variables. The renaming reaches the innermost rule's
    // conditions only via their conjunction set, and the substitution
    // applied used to return that set unaltered: the conditions kept X and
    // Y, whereas the consequence got the renamed pair, which no condition
    // bound any more. Subsequently, `j is up` wrote a rule that made a
    // witness for each, and `a p b, b q a` derived `?? r ??`.
    //
    // `.semi-naive on`: on an engine that does not rebuild the set, the
    // classic pass in check mode generates fresh witnesses on every pass
    // and never reaches a stable state, thus the defect would show as a
    // test that does not end rather than as a failed check.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive on
(K is on) => ((J is up) => ((X p Y, Y q X) => (X r Y)))
k is on
j is up
a p b
b q a
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        const auto answers = collect_answers(collector);
        CHECK(answers_contain(collector, "a r b"));
        for (const auto& answer : answers)
            CHECK(answer.find("??") == std::string::npos); });
}

TEST_CASE("derived rules: a switch over a generator claims the generator once")
{
    // `sw is on` does not bind anything within the generator located
    // beneath the switch, hence building it lands on the node that the
    // switch mentions, and build_rule claims an alpha-renamed copy, or the
    // copy previously claimed during an earlier pass, unless the generator
    // holds a collection (see below). The innermost rule of the copy
    // possesses a conjunction set of its own, with H renamed, and this rule
    // lacks an entry in the template-variable store: the store records the
    // variables of hash components only, and a set is not one. When
    // compared via its node, the rule would never be identified as the
    // prior copy, each pass would write another, and the run would never
    // terminate.
    //
    // The set used to come back unaltered. That ended the run and left H
    // free within the copy's conditions: `b q m, b p m` derived what
    // only `b q k, b p k` may. One rule further down, no derivation
    // occurred whatsoever.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto switch_on = [&](const std::string& generator)
        {
            claims_once(collector, interactive, generator, "sw is on");
            interactive.process("now go k");
        };

        SUBCASE("a contradiction rule")
        {
            switch_on("(K is on) => ((G go H) => (((X q H), (X p H)) => !))");

            collector.clear();
            process_lines(interactive, R"(
b q m
b p m
)");
            CHECK_FALSE(has_contradiction(collector));

            collector.clear();
            process_lines(interactive, R"(
b q k
b p k
)");
            CHECK(has_contradiction(collector));
        }
        SUBCASE("a ground consequence")
        {
            switch_on("(K is on) => ((G go H) => (((X q H), (X p H)) => (alarm is on)))");

            process_lines(interactive, R"(
b q m
b p m
)");
            collector.clear();
            interactive.process("A is on");
            CHECK_FALSE(answers_contain(collector, "alarm is on"));

            process_lines(interactive, R"(
b q k
b p k
)");
            collector.clear();
            interactive.process("A is on");
            CHECK(answers_contain(collector, "alarm is on"));
        }
        SUBCASE("one rule deeper")
        {
            switch_on("(K is on) => ((G go H) => ((a is up) => (((X q H), (X p H)) => !)))");
            interactive.process("a is up");

            collector.clear();
            process_lines(interactive, R"(
b q m
b p m
)");
            CHECK_FALSE(has_contradiction(collector));

            collector.clear();
            process_lines(interactive, R"(
b q k
b p k
)");
            CHECK(has_contradiction(collector));
        }

        // In the next two, the variables reside within a collection contained
        // inside a SET CONSTANT. A set constant is a hash node lacking a fact
        // structure, and the store maintains no entry for it. Each rule that
        // the switch writes possesses its own set constant, because the
        // collection within it is rebuilt -- either with the renamed
        // variables or through the construction as one of its own -- so
        // compared by its node, as `{a b}` is, the rule would never be
        // identified, and every pass would write an additional one.
        //
        // The set constant used to return unaltered, thereby terminating
        // the run and leaving H free in it: `b p k` derived
        // `b likes {@{(Y q H)}}`.
        SUBCASE("a rule inside a set constant")
        {
            switch_on("(K is on) => ((G go H) => ((X p H) => (X likes {(((Y q H), (Y p H)) => !)})))");
            interactive.process("b p k");

            collector.clear();
            interactive.process("b likes S");
            const auto answers = collect_answers(collector);
            REQUIRE(answers.size() == 1);
            CHECK(answers.front().find("(Y q k)") != std::string::npos);
            CHECK(answers.front().find("(Y p k)") != std::string::npos);
            CHECK(answers.front().find('H') == std::string::npos);
        }
        SUBCASE("a collection inside a set constant")
        {
            switch_on("(K is on) => ((G go H) => ((X p H) => (X likes {@{(Y q H)}})))");
            interactive.process("b p k");

            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes {@{(Y q k)}}"));
            CHECK(collect_answers(collector).size() == 1);
        }

        // The construction rebuilds both collections as its own, and the
        // outer one keeps its kind: it was written as a collection around a
        // collection, thus its kind is known. The generated rule holds
        // both, and what it derives holds their terms.
        SUBCASE("a collection inside a collection")
        {
            switch_on("(K is on) => ((G go H) => ((X p H) => (X likes @{@{(Y q H)}})))");
            interactive.process("b p k");

            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes @{@{(Y q k)}}"));
            CHECK(collect_answers(collector).size() == 1);
        }

        // Two collections sharing identical members, each keeping Y in every
        // rule positioned beneath the switch. The copy must hold two such
        // collections, just as the mention does (see the switch over a plain
        // rule below), and so does the rule the copy writes for k: its
        // collections are rebuilt with the binding, each under a distinct
        // recipe, ensuring the next pass lands on the same pair. That level
        // is counted with auto-run off as well. The answers correspond to the
        // typed `(X p k) => (X likes @{(Y q k)} @{(Y q k)})`.
        SUBCASE("two collections with the same members")
        {
            claims_once(collector, interactive, "(K is on) => ((G go H) => ((X p H) => (X likes @{(Y q H)} @{(Y q H)})))", "sw is on");

            interactive.process(".auto-run"); // off
            interactive.process("now go k");
            for (int pass = 0; pass < 3; ++pass)
                interactive.process(".run-once");
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(listed_rules(collector) == 3);
            interactive.process(".auto-run"); // on again

            interactive.process("b p k");
            collector.clear();
            interactive.process("b likes S");
            const auto answers = collect_answers(collector);
            REQUIRE(answers.size() == 2);
            for (const auto& answer : answers)
                CHECK(answer == "b likes @{(Y q k)}");
        } });
}

TEST_CASE("derived rules: a rule that binds nothing below it claims its copy once")
{
    // The two other shapes of the switch idiom, with a collection nested
    // within another in the rule below: a switch over a plain rule, and a
    // generator of a generator, with A and B appearing nowhere within the
    // generator it writes. Each rule written holds collections of its own,
    // where the mention holds the rule's text (refer to the switch over a
    // generator above), and the subsequent pass must find it all the same.
    // Previously, the collection returned unchanged, carrying the mention's
    // variables, such that `b p k` derived `b likes @{@{(Z q Y)}}` and
    // `b likes @{@{(Y q H)}}`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a switch over a plain rule")
        {
            claims_once(collector, interactive, "(K is on) => ((X p Y) => (X likes @{@{(Z q Y)}}))", "sw is on");
            interactive.process("b p k");

            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes @{@{(Z q k)}}"));
            CHECK(collect_answers(collector).size() == 1);
        }
        SUBCASE("a generator of a generator")
        {
            claims_once(collector, interactive, "(A rel B) => ((G go H) => ((X p H) => (X likes @{@{(Y q H)}})))", "m rel n");
            interactive.process("now go k");
            interactive.process("b p k");

            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes @{@{(Y q k)}}"));
            CHECK(collect_answers(collector).size() == 1);
        }

        // Two collections sharing identical members are distinct terms, as
        // the parser writes them. The construction rebuilds each under its
        // individual recipe. If the two were one, the rule written would say
        // `X likes @{Y}` with a single object where the mention has two, the
        // subsequent pass would not find it, and each pass would write one
        // more.
        SUBCASE("two collections with the same members")
        {
            claims_once(collector, interactive, "(K is on) => ((X p Y) => (X likes @{Y} @{Y}))", "sw is on");
            interactive.process("b p k");

            // Each object transforms into the set
            // constant {k}.
            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes {k}"));
            CHECK(collect_answers(collector).size() == 1);
        } });
}

TEST_CASE("derived rules: a ground condition in a generated generator's conjunction is a pattern")
{
    // The conjunction rebuilt for the inner rule is a NEW set node whose id
    // nobody can know before it is created, hence the members it generates
    // are accessed via the set, rather than through the list of ground
    // parts gathered beforehand. Absent this, `k is on` would constitute a
    // claim the moment the rule is written, and `a p k` alone would derive
    // `a q k`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((S is set) => ((H is on, X p H) => (X q H)))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is on");
        REQUIRE_FALSE(answers_contain(collector, "k is on"));

        // m is on, yet the rule is about k.
        process_lines(interactive, R"(
z is set
m is on
a p m
a p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());

        interactive.process("k is on");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q k"));
        CHECK_FALSE(answers_contain(collector, "a q m")); });
}

TEST_CASE("derived rules: a conjunction rebuilt for a generated rule does not claim an asserted condition as a member")
{
    // The conjunction of the inner rule is rebuilt as a new set node, and
    // each membership fact within it arises alongside the rule, including
    // that of a member that had previously existed. Since `k partof x` was
    // asserted prior to `now go k`, it stays a claim, whereas its membership
    // in the conjunction is rule structure. A construction that marked
    // solely the membership facts of members newly introduced would not
    // affect this one; it would still stand as a claim: `S in O` would
    // answer it, and a rule concerning `in` would derive
    // `(k partof x) flagged yes` from nothing except the rule being written.
    // The typed rule `(k is on) => ((k partof x, I q k) => (k is green))`
    // derives nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
k partof x
(G go H) => ((H is on) => ((H partof x, I q H) => (H is green)))
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("A partof B");
        CHECK(answers_contain(collector, "k partof x")); });
}

TEST_CASE("derived rules: a ground condition in a conjunction nested in a generated condition is a pattern")
{
    // In the scenarios described above, the conjunction is the condition of a
    // rule inside a generated rule's consequence. Here, it is a member of the
    // generated rule's condition itself, rebuilt alongside it, and the walk
    // that marks a rule's ground parts never examined its interior:
    // `k is red` became a claim from the moment `now go k` wrote the rule,
    // thus the rule fired without it and derived `a r k`. The typed rule
    // `(((k is red), (X p k)), (X q k)) => (X r k)` derives nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((((H is red), (X p H)), (X q H)) => (X r H))
now go k
a p k
a q k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (k is red)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        interactive.process("k is red");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(answers_contain(collector, "a r k")); });
}

TEST_CASE("derived rules: a nested conjunction of ground conditions in a generated condition is walked as well")
{
    // The parser builds a rule's conjunction as a collection regardless of
    // its members, and the rebuild performs the same action, so the walk
    // traverses these members via the collection and marks them and their
    // membership facts as patterns. Unmarked, `k is red` and `k is big` were
    // claims, and the rule fired on `a q k` by itself. Without any variable
    // present among its members, the nested set used to be rebuilt as a set
    // constant instead, where the membership facts constitute the set
    // itself: `S in O` answered `(k is red) in {(k is red) (k is big)}`, and
    // a rule over `in` derived `(k is red) flagged yes`. The typed rule
    // `(((k is red), (k is big)), (X q k)) => (X r k)` carries out none of
    // these steps. The membership facts of the outermost conjunction set are
    // not queried: a typed rule leaves those unmarked as well.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
(G go H) => ((((H is red), (H is big)), (X q H)) => (X r H))
now go k
a q k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (k is red)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        collector.clear();
        interactive.process("S flagged O");
        CHECK_FALSE(answers_contain(collector, "(k is red) flagged yes"));
        CHECK_FALSE(answers_contain(collector, "(k is big) flagged yes"));

        collector.clear();
        interactive.process("S in O");
        for (const auto& answer : collect_answers(collector))
        {
            CHECK(answer.find("(k is red) in ") == std::string::npos);
            CHECK(answer.find("(k is big) in ") == std::string::npos);
        }

        process_lines(interactive, R"(
k is red
k is big
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(answers_contain(collector, "a r k")); });
}

TEST_CASE("derived rules: a conjunction nested in a generated condition does not claim its members")
{
    // The nested set constitutes a fresh collection, constructed via the
    // rule, and each of its membership facts is likewise built with the
    // rule. The typed rule marks them as patterns; the generated one left
    // them as claims, thus `S in O` answered
    // `(k is red) in {(k is red) (X p k)}` and a rule over `in` derived
    // `(k is red) flagged yes`. Membership facts within the outermost
    // conjunction set are not asked about: a typed rule also leaves those
    // unmarked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
(G go H) => ((((H is red), (X p H)), (X q H)) => (X r H))
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 3);

        collector.clear();
        interactive.process("S flagged O");
        CHECK_FALSE(answers_contain(collector, "(k is red) flagged yes"));

        collector.clear();
        interactive.process("S in O");
        for (const auto& answer : collect_answers(collector))
            CHECK(answer.find("(k is red) in ") == std::string::npos); });
}

TEST_CASE("derived rules: a negated condition in a conjunction nested in a generated condition stays negated")
{
    // The derived rule's conditions are rebuilt via rebuild_condition, yet a
    // conjunction nested within one of those conditions goes on to
    // instantiate_container, which rebuilds the condition set of a nested
    // rule as well and reasserts the negation tags of its members. This case
    // is that path for a negated member; the cases further up have their
    // negated condition directly present in the derived rule's conditions or
    // within a nested rule. Absent the tag, the condition reads
    // `(X blocks k)`, and c, which blocks k, works.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The blocker comes first: in check mode, a negation that
        // comes to hold after a fact has been derived under it is
        // rejected.
        process_lines(interactive, R"(
c blocks k
(G go H) => ((((X q H), ¬(X blocks H)), (X p H)) => (X works H))
now go k
b p k
b q k
c p k
c q k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S works O");
        CHECK(answers_contain(collector, "b works k"));
        CHECK_FALSE(answers_contain(collector, "c works k"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "¬(X blocks k)")); });
}

TEST_CASE("derived rules: recording a typed contradiction does not assert a negated member of a nested conjunction")
{
    // Rebuilding a conjunction set is part of the construction of a rule and
    // nothing beyond that. When a contradiction is recorded, it instantiates
    // the conditions of the rule that fired as well, and it skips a negated
    // condition solely at the top level. If the rebuild were to execute
    // there as well, the nested set would be rebuilt one member at a time:
    // `b blocks k`, the instance of `¬(X blocks k)`, would be asserted as an
    // axiom and tagged as negated, and a rule concerning `blocks` would
    // derive `b stuck k` from it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
c blocks k
(((X q k), ¬(X blocks k)), (X p k)) => !
b p k
b q k
)");
        interactive.run(true, false, false);
        REQUIRE(has_contradiction(collector));

        collector.clear();
        interactive.process("S blocks O");
        CHECK(answers_contain(collector, "c blocks k"));
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process(".explain (b blocks k)");
        CHECK(any_output_contains(collector, "Fact is not asserted"));

        interactive.process("(A blocks B) => (A stuck B)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S stuck O");
        CHECK(answers_contain(collector, "c stuck k"));
        CHECK(collect_answers(collector).size() == 1); });
}

TEST_CASE("derived rules: recording a generated contradiction does not assert a negated member of a nested conjunction")
{
    // The generated twin of the case mentioned earlier. Its nested set was
    // constructed by the rebuild while the rule was derived, which is right,
    // and the record instantiates it a second time after the rule fires,
    // which is incorrect: the rebuild must distinguish between the two based
    // on when it runs, not on the set it receives.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
c blocks k
(G go H) => ((((X q H), ¬(X blocks H)), (X p H)) => !)
now go k
b p k
b q k
)");
        interactive.run(true, false, false);
        REQUIRE(has_contradiction(collector));

        collector.clear();
        interactive.process("S blocks O");
        CHECK(answers_contain(collector, "c blocks k"));
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process(".explain (b blocks k)");
        CHECK(any_output_contains(collector, "Fact is not asserted")); });
}

TEST_CASE("derived rules: recording a contradiction does not claim the members of a nested conjunction")
{
    // The identical path, devoid of negation. Upon rebuilding there, the
    // nested set would reappear as a new collection holding `b q k` and
    // `b r k`, and since no rule is constructed, nothing would mark its
    // membership facts as patterns: a rule concerning `in` would derive
    // `(b q k) flagged yes` and `(b r k) flagged yes`. The record of the
    // contradiction exists as a distinct set, and its membership facts,
    // including `(b p k) in {…}`, are not asked about.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
(((X q k), (X r k)), (X p k)) => !
b p k
b q k
b r k
)");
        interactive.run(true, false, false);
        REQUIRE(has_contradiction(collector));

        collector.clear();
        interactive.process("S flagged O");
        CHECK_FALSE(answers_contain(collector, "(b q k) flagged yes"));
        CHECK_FALSE(answers_contain(collector, "(b r k) flagged yes"));

        collector.clear();
        interactive.process("S in O");
        for (const auto& answer : collect_answers(collector))
        {
            CHECK(answer.find("(b q k) in ") == std::string::npos);
            CHECK(answer.find("(b r k) in ") == std::string::npos);
        } });
}

TEST_CASE("derived rules: order-theoretic properties as generators")
{
    // The application from mkdocs/docs/rule-generators.md. What makes a
    // relation an order is a statement ABOUT the relation, so the properties
    // are generators and declaring a relation installs its rules. Two
    // relations declared the same way must stay apart -- each generated rule
    // carries its predicate.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is partialorder) => ((X R Y, Y R Z) => (X R Z))
(R is partialorder) => ((X R Y, Y R X) => (X sameas Y))
divides is partialorder
contains is partialorder
two divides four
four divides eight
red contains pink
pink contains rose
)");
        interactive.run(true, false, false);

        // Two generators plus two rules per declared relation.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 6);

        collector.clear();
        interactive.process("S divides O");
        CHECK(answers_contain(collector, "two divides eight"));
        CHECK_FALSE(answers_contain(collector, "red divides rose"));

        collector.clear();
        interactive.process("S contains O");
        CHECK(answers_contain(collector, "red contains rose"));

        // Antisymmetry has nothing to fire on: the data is acyclic.
        collector.clear();
        interactive.process("S sameas O");
        CHECK(collect_answers(collector).empty());

        // ... until it does.
        interactive.process("eight divides two");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S sameas O");
        CHECK(answers_contain(collector, "two sameas four")); });
}

TEST_CASE("derived rules: a modal system's axioms as data")
{
    // The other application from the page: a normal modal logic is named by
    // the axiom schemas it accepts, so WHICH schemas a system accepts becomes
    // ordinary data and every system gets its own inference rules from the
    // same graph.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(S accepts axiomT) => ((A necessaryin S) => (A holdsin S))
(S accepts axiomD) => ((A necessaryin S) => (A possiblein S))
kt accepts axiomT
kd accepts axiomD
p necessaryin kt
q necessaryin kd
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A holdsin S");
        CHECK(answers_contain(collector, "p holdsin kt"));
        // kd does not accept T, so nothing HOLDS there.
        CHECK_FALSE(answers_contain(collector, "q holdsin kd"));

        collector.clear();
        interactive.process("A possiblein S");
        CHECK(answers_contain(collector, "q possiblein kd"));
        CHECK_FALSE(answers_contain(collector, "p possiblein kt"));

        // Two generators and one rule per system.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 4); });
}

TEST_CASE("derived rules: a container in a switched consequence holds the variables of the rule written")
{
    // A rule generator that substitutes NOTHING into its inner rule -- the
    // switch shape -- lands the rebuild on the exact node that the outer rule
    // merely MENTIONS (refer to "a switch turns a rule on" earlier), and
    // build_rule performs an alpha-renaming of the rule, unless the rule
    // holds a collection, which the construction builds as one of its own.
    // Either way, a container of the rule written must hold that rule's
    // variables. By holding the variable originating from the rule it was
    // written from, it would be derived with that variable unbound:
    //
    //     (K is on) => ((X p Y) => (X likes {Y}))
    //     Answer: a likes @{Y}      <- the generator's own variable, unbound
    //
    // The same rule TYPED derives `a likes {b}`, and a generated rule has to
    // behave like the rule it generates.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
k is on
a p b
c p d
(K is on) => ((X p Y) => (X likes {Y}))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "a likes {b}"));
        CHECK(answers_contain(collector, "c likes {d}"));
        CHECK(collect_answers(collector).size() == 2);

        // Not the generator's template variable, in any spelling.
        CHECK_FALSE(any_output_contains(collector, "@{Y}"));

        // The fixpoint is reached: the construction rebuilds the container
        // under a recipe, which remains the identical node across each
        // pass, thus a further run finds the generated rule, derives nothing,
        // and writes no rule.
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t rules_before = listed_rules(collector);

        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == rules_before);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("derived rules: a generated rule writing INTO a container fills one container that every binding shares")
{
    // The counterpart: `Y in @{X}` says something CONCERNING the
    // container, thus its identity must persist through substitution. The
    // generated rule holds a bucket of its own, and each fact it derives
    // is placed within that bucket's term, a single node common to every
    // binding.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
k is on
alice reported bug1
bob reported bug2
(K is on) => ((X reported Y) => (Y in @{X}))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "bug1 in @{bug1 bug2}"));
        CHECK(answers_contain(collector, "bug2 in @{bug1 bug2}"));
        CHECK(collect_answers(collector).size() == 2);

        // It reaches convergence: during the next pass, the rule is
        // constructed over the identical bucket, the collection of its
        // recipe, and is found.
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t rules_before = listed_rules(collector);

        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == rules_before); });
}

TEST_CASE("derived rules: a generator that substitutes needs no renaming at all")
{
    // The control for both cases above. When the outer rule substitutes into
    // the inner one, the rebuild lands on a node of its own and the renaming
    // path is never entered -- this shape worked before and has to keep
    // working, which is what tells the two mechanisms apart.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p collects q
a p b
c p d
(P collects Q) => ((X P Y) => (X Q {Y}))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q {b}"));
        CHECK(answers_contain(collector, "c q {d}"));
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("derived rules: each rule a generator writes has a bucket of its own")
{
    // A bucket within a generated rule is a collection of that rule's text:
    // the construction builds it for the rule it produces, holding the outer
    // binding as a ground member, while the rule's firings write into the
    // term of that bucket. The generator's own bucket was shared by each
    // rule it produced, thus `t` and `u` occupied a single bucket, with H
    // never substituted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the outer binding is a member")
        {
            process_lines(interactive, R"(
(G go H) => ((X G Y) => (Y in @{H}))
t go k
u go m
a t bug1
b u bug2
)");
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"bug1 in @{k bug1}", "bug2 in @{m bug2}", "k in @{k bug1}", "m in @{m bug2}"});
        }
        SUBCASE("only the inner variable is a member")
        {
            process_lines(interactive, R"(
(G go H) => ((X G Y) => (Y in @{Y}))
t go k
u go m
a t b1
c u b2
)");
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"b1 in @{b1}", "b2 in @{b2}"});
        }
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a collection in a generated condition takes the outer binding")
{
    // In the condition of a rule that has been generated, a literal is built
    // by the construction with the generator's binding substituted,
    // resulting in the same kind that the typed twin would possess: `{k}`
    // denotes the set constant, which holds `k in {k}` just as it does for
    // the typed rule. Kept as the generator's own literal, the condition
    // read `(X in @{X H})`, with H remaining unbound, and matched nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a set constant, beside its typed twin")
        {
            process_lines(interactive, R"(
k in {k}
m in {k m}
(G go H) => ((X in {H}) => (X G yes))
t go k
(X in {k}) => (X u yes)
)");
            collector.clear();
            interactive.process("S t O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k t yes"});
            collector.clear();
            interactive.process("S u O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k u yes"});
        }
        SUBCASE("a collection whose members are all bound becomes the set constant")
        {
            process_lines(interactive, R"(
(G go H) => ((X in @{H m}) => (X G yes))
t go k
)");
            collector.clear();
            interactive.process("S t O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"k t yes", "m t yes"});
        }
        SUBCASE("a value the generator binds")
        {
            process_lines(interactive, R"(
d has @{x}
(S has C) => ((X in @{C}) => (X wrapped S))
)");
            collector.clear();
            interactive.process("S wrapped O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"@{x} wrapped d"});
        }
        SUBCASE("a negated condition")
        {
            process_lines(interactive, R"(
(G go H) => ((X p Y, ¬(Y in @{H})) => (X G Y))
t go k
a p k
a p m
)");
            collector.clear();
            interactive.process("S t O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a t m"});
        }
        SUBCASE("a membership the condition mentions")
        {
            process_lines(interactive, R"(
(k in {k}) is noted
(G go H) => (((Y in @{H}) is noted) => (Y G yes))
t go k
)");
            collector.clear();
            interactive.process("S t O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k t yes"});
        }
        SUBCASE("a collection no binding changes stays a collection of the rule")
        {
            // `@{a b}` constitutes a collection of the generated rule's
            // text, as it is of the typed rule positioned alongside it: its
            // memberships consist of rule text, thus neither condition
            // matches anything.
            process_lines(interactive, R"(
(G go H) => ((X in @{a b}) => (X G yes))
t go k
(X in @{a b}) => (X u yes)
)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 3);
            collector.clear();
            interactive.process("S t O");
            CHECK(collect_answers(collector).empty());
        } });
}

TEST_CASE("derived rules: a collection in predicate position is a relation per binding")
{
    // A literal in predicate position constitutes a collection of the
    // rule's text in the same manner as any other: every firing writes the
    // term associated with its individual binding, a relation that holds
    // `b` as data. The two derived facts display an identical predicate and
    // name two distinct relations, thus a rule that joins on the predicate
    // does not relate them, just as two bindings of `(X likes @{bucket})`
    // name two separate collections. When a single shared relation is
    // meant, a named node or the set constant `{b}` explicitly indicates
    // this.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (Y @{b} X)
a p c
d p e
(c P A, e P D) => (A shares-relation D)
)");
        collector.clear();
        interactive.process("S shares-relation O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector) == std::vector<std::string>(2, "b in @{b}"));
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: rules a generator writes do not share a collection's term")
{
    // `t` and `u` each write the rule `(X p k) => (X <G> @{(X q k) (Z r k)})`
    // using a distinct collection; a firing by either writes the term of that
    // collection, thus `b t ...` and `b u ...` name two terms, while `torn`
    // joins nothing, just as with the two typed rules. The generator's
    // collection was written into the data by both, which made the two facts
    // refer to a single node.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("generated")
        {
            process_lines(interactive, R"(
(G go H) => ((X p H) => (X G @{(X q k) (Z r k)}))
(X t C, X u C) => (X torn C)
t go k
u go k
b p k
)");
        }
        SUBCASE("typed")
        {
            process_lines(interactive, R"(
(X p k) => (X t @{(X q k) (Z r k)})
(X p k) => (X u @{(X q k) (Z r k)})
(X t C, X u C) => (X torn C)
b p k
)");
        }
        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());
        // A single term, holding the instantiated member and the one
        // with the fresh variable, in either order.
        collector.clear();
        interactive.process("b t O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front().find("(b q k)") != std::string::npos);
        CHECK(answers.front().find("(Z r k)") != std::string::npos); });
}

TEST_CASE("derived rules: a generator that mentions a rule with a collection ends")
{
    // The mentioned rule's collection is built in the same manner as a
    // construction builds it: `@{H}` under `t go k` is the set constant
    // `{k}`, so the next pass finds the mention it wrote. It was built afresh
    // with each pass, each time a distinct node, and the run never ended.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto counts = counts_over_passes(collector, interactive, R"(
(G go H) => (((X G Y) => (Y q @{H})) noted yes)
t go k
u go m
)");
        CHECK(counts == std::vector<std::size_t>(4, counts.front()));

        collector.clear();
        interactive.process("S noted O");
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("derived rules: a term a generator binds stays a value of the rule it writes")
{
    // A binding is a value: the term of an accumulator to which a
    // generator binds C is written into the generated rule as that node,
    // each member that the term contains provides an answer, including the
    // ground members within the typed rule's collection, and the firing
    // writes into that term. The binding was read as the rule's own
    // collection, thus the generated rule printed `@{X Y}`, and only the
    // members that a firing had introduced provided answers.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("in a generated rule")
        {
            // Two typed accumulators, each bearing a unique term: a member
            // of both answers once within each.
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
(X filed Y) => (Y in @{bug1 bug2})
alice reported bug3
bob filed bug3
(Y in C) => ((X p Y) => (X in C))
a p bug3
)");
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            std::vector<std::string> expected;
            for (const std::string member : {"a", "bug1", "bug2", "bug3"})
                expected.insert(expected.end(), 2, member + " in @{bug1 bug2 bug3 a}");
            CHECK(answers == expected);

            // After generators wrote into its term, entering a typed
            // accumulator once more locates the rule that resides
            // there.
            trigger_adds_no_rule(collector, interactive, "(X filed Y) => (Y in @{bug1 bug2})");
        }
        SUBCASE("in a generated generator")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
(Y in C) => ((G go H) => ((X G Y) => (X in C)))
t go k
a t bug3
)");
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"a in @{bug1 bug2 bug3 a}", "bug1 in @{bug1 bug2 bug3 a}", "bug2 in @{bug1 bug2 bug3 a}", "bug3 in @{bug1 bug2 bug3 a}"});
        }
        SUBCASE("in the condition of a generated generator")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
(Y in C) => ((G go H) => ((Z in C) => (Z G H)))
t go k
)");
            collector.clear();
            interactive.process("S t O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"bug1 t k", "bug2 t k", "bug3 t k"});
        }
        SUBCASE("in a condition, for two accumulators")
        {
            // Each pair of members within a single term is identical, and
            // no member of one term is the same as any member that only
            // the other term holds.
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
(X filed Y) => (Y in @{bug1 bug2})
alice reported bug3
bob filed bug4
(Y in C) => ((X in C) => (X same Y))
)");
            collector.clear();
            interactive.process("S same O");
            const auto answers = collect_answers(collector);
            CHECK(answers.size() == 14);
            CHECK(std::ranges::find(answers, "bug1 same bug3") != answers.end());
            CHECK(std::ranges::find(answers, "bug2 same bug4") != answers.end());
            CHECK(std::ranges::find(answers, "bug3 same bug4") == answers.end());
            CHECK(std::ranges::find(answers, "bug4 same bug3") == answers.end());
        }
        SUBCASE("for accumulators over named members")
        {
            process_lines(interactive, R"(
(X is-a-role yes) => (X in @{roles})
(X is-a-team yes) => (X in @{teams})
(X is-a-unit yes) => (X in @{teams})
boss is-a-role yes
boss is-a-unit yes
(Y in C) => ((X reports-to Y) => (X in C))
ann reports-to boss
)");
            collector.clear();
            interactive.process("ann in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"ann in @{roles boss ann}", "ann in @{teams boss ann}"});
            trigger_adds_no_rule(collector, interactive, "(X is-a-unit yes) => (X in @{teams})");
        }
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a generated generator that feeds its own binding settles")
{
    // `b t a` and `a t b` cause each member in the term to re-bind the
    // generated generator; the term holds each member exactly once and the
    // run ends.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
(Y in C) => ((G go H) => ((X G Y) => (X in C)))
t go k
a t bug3
b t a
a t b
)");
        collector.clear();
        interactive.process("S in O");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"a in @{bug1 bug2 bug3 a b}", "b in @{bug1 bug2 bug3 a b}", "bug1 in @{bug1 bug2 bug3 a b}", "bug2 in @{bug1 bug2 bug3 a b}", "bug3 in @{bug1 bug2 bug3 a b}"});
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a collection bound as a value is not rebuilt")
{
    // A collection that reaches a generated rule via a binding
    // constitutes a value: the rule names that node, no matter its
    // position, and the data answers concerning it as with any node. No
    // entity builds a term for it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers_of = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            return answers;
        };
        SUBCASE("a collection written as data, in a consequence and a condition")
        {
            process_lines(interactive, R"(
x likes @{a b}
(X likes C) => ((Y q X) => (Y has C))
(X likes C) => ((Z in C) => (Z liked-by X))
(A has C, B likes C) => (A shares B)
d q x
)");
            CHECK(answers_of("S shares O") == std::vector<std::string>{"d shares x"});
            CHECK(answers_of("S liked-by O") == std::vector<std::string>{"a liked-by x", "b liked-by x"});
        }
        SUBCASE("the term of a typed accumulator")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
(Y in C) => ((X p Y) => (X q C))
a p bug3
)");
            CHECK(answers_of("S q O") == std::vector<std::string>{"a q @{bug1 bug2 bug3}"});
        }
        SUBCASE("the term of a generated bucket")
        {
            process_lines(interactive, R"(
(G go H) => ((X G Y) => (Y in @{H}))
t go k
a t bug1
(Y in C) => ((X p Y) => (X q C))
b p bug1
)");
            CHECK(answers_of("S q O") == std::vector<std::string>{"b q @{k bug1}"});
        }
        SUBCASE("the term of a mentioned membership")
        {
            process_lines(interactive, R"(
(X p Y) => ((Y in @{X}) is noted)
a p k
((Y in C) is noted) => ((Z r Y) => (Z s C))
b r k
)");
            CHECK(answers_of("S s O") == std::vector<std::string>{"b s @{k}"});
        }
        SUBCASE("a collection a generated rule writes into")
        {
            process_lines(interactive, R"(
d has @{seed}
(S has C) => ((X in C, X p Y) => (Y in C))
seed p a
a p b
b p c
)");
            CHECK(answers_of("S in O") == std::vector<std::string>{"a in @{seed a b c}", "b in @{seed a b c}", "c in @{seed a b c}", "seed in @{seed a b c}"});
        }
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a term bound into a rule keeps naming its value across .save and .load")
{
    // The premise of the accumulator is removed; the term, which the
    // loaded network continues to retain, is designated by the rules
    // authored by the generator.
    const auto file = fs::temp_directory_path() / "zelph_derived_rules_bound_term_test.bin";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
(Y in C) => ((X p Y) => (X q C))
a p bug3
.remove reported
)");
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");
        interactive.process("d p bug3");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"a q @{bug1 bug2 bug3}", "d q @{bug1 bug2 bug3}"}); });

    fs::remove(file);
}

TEST_CASE("derived rules: a data collection bound into generated rules stays a value whatever they write into it")
{
    // A generated rule over a data collection D leaves its statement's
    // membership on D: `X in D` using a variable member, or the pattern
    // `k in D`. If such a membership were interpreted as evidence that D
    // was written with a rule, D would qualify as a rule's own collection
    // as soon as the first generated rule had written into it, and what
    // the subsequent rule over D or over a collection E with identical
    // members is would depend on the order of rules: one generated rule
    // claiming another, an answer lost, a rule written one run late, two
    // rules printed as one. A collection is a rule's own exactly when its
    // identifier indicates as much, and no membership a rule leaves on D
    // contributes to this determination.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers_of = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            return answers;
        };
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        const auto two_generators = [&](const std::string& first, const std::string& second)
        {
            process_lines(interactive, "d has @{x}\ne has @{y}\n" + first + "\n" + second + "\na r b\n");
            CHECK(rules() == 6);
            CHECK(answers_of("S in O") == std::vector<std::string>{"a in @{x k a}", "a in @{y k a}", "x in @{x k a}", "y in @{y k a}"});
        };
        SUBCASE("two generators over two collections, one order")
        {
            two_generators("(S has C) => ((X q Y) => (k in C))", "(S has C) => ((X r Y) => (X in C))");
        }
        SUBCASE("two generators over two collections, the other order")
        {
            two_generators("(S has C) => ((X r Y) => (X in C))", "(S has C) => ((X q Y) => (k in C))");
        }
        SUBCASE("a generated rule whose condition is the membership another one writes")
        {
            process_lines(interactive, R"(
d has @{x}
e has @{y}
(S has C) => ((k in C) => (S flagged yes))
(S has C) => ((X r Y) => (X in C))
a r b
)");
            CHECK(rules() == 6);
            CHECK(answers_of("S in O") == std::vector<std::string>{"a in @{x k a}", "a in @{y k a}", "x in @{x k a}", "y in @{y k a}"});
            CHECK(answers_of("S flagged O").empty());

            interactive.process("k r z");
            CHECK(rules() == 6);
            CHECK(answers_of("S flagged O") == std::vector<std::string>{"d flagged yes", "e flagged yes"});
            CHECK(answers_of("S in O") == std::vector<std::string>{"a in @{x k a}", "a in @{y k a}", "k in @{x k a}", "k in @{y k a}", "x in @{x k a}", "y in @{y k a}"});
        }
        SUBCASE("the membership nested in a statement the generated rule writes")
        {
            process_lines(interactive, R"(
d has @{x}
e has @{y}
(S has C) => ((X q Y) => ((X in C) is noted))
a q b
)");
            CHECK(rules() == 3);
            CHECK(answers_of("S is O") == std::vector<std::string>{"(a in @{x a}) is noted", "(a in @{y a}) is noted"});
        }
        SUBCASE("the membership as a member of a literal of the generated rule")
        {
            process_lines(interactive, R"(
d has @{x}
e has @{y}
(S has C) => ((X q Y) => (X likes @{(k in C)}))
(S has C) => ((X r Y) => (X in C))
a r b
)");
            CHECK(rules() == 6);
            CHECK(answers_of("S in O") == std::vector<std::string>{"(k in @{x k a}) in {(k in @{x k a})}", "(k in @{y k a}) in {(k in @{y k a})}", "a in @{x k a}", "a in @{y k a}", "x in @{x k a}", "y in @{y k a}"});
        }
        SUBCASE("a pattern membership the generated rule derives")
        {
            process_lines(interactive, R"(
d has @{x}
e has @{y}
(S has C) => ((X q Y) => (k in C))
a q b
)");
            CHECK(rules() == 3);
            CHECK(answers_of("S in O") == std::vector<std::string>{"k in @{x k}", "k in @{y k}", "x in @{x k}", "y in @{y k}"});
        }
        SUBCASE("a pattern membership in the generated rule's condition")
        {
            // The expression `k in @{x}` constitutes a distinct
            // literal and refers to a new collection, not D.
            process_lines(interactive, R"(
d has @{x}
e has @{y}
(S has C) => ((k in C) => (z ok S))
k in @{x}
)");
            CHECK(rules() == 3);
            CHECK(answers_of("S ok O").empty());
        }
        SUBCASE("a typed rule whose bucket holds the pattern's member")
        {
            process_lines(interactive, R"(
d has @{x}
(S has C) => ((X q Y) => (k in C))
(X q Y) => (k in @{k})
a q b
)");
            CHECK(rules() == 3);
            CHECK(answers_of("S in O") == std::vector<std::string>{"k in @{k}", "k in @{x k}", "x in @{x k}"});
        }
        SUBCASE("a variable membership among the generated rule's conditions")
        {
            process_lines(interactive, R"(
d has @{x}
e has @{y}
(S has C) => ((X in C, X q k) => (X member-of S))
x q k
y q k
)");
            CHECK(rules() == 3);
            CHECK(answers_of("S member-of O") == std::vector<std::string>{"x member-of d", "y member-of e"});
        }
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a nested rule a firing writes into a collection or a mention is not answered")
{
    // The conditions of the nested rule hold variables from the outer
    // rule, which the firing substitutes, along with variables unique to
    // itself: it is a rule's text, partially substituted, and neither a
    // rule in force nor data. `b s k` is still answered: a firing asserts
    // the facts nested in what it writes, with the consequence of the
    // nested rule being one of them.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("in a collection")
        {
            process_lines(interactive, R"(
(X p Y) => (X likes @{((X q Y, X r Y) => (X s Y))})
b p k
c q d
c r d
)");
            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process("S s O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b s k"});
        }
        SUBCASE("in a mention")
        {
            process_lines(interactive, R"(
(X p Y) => (((X q Y, X r Y) => (X s Y)) is noted)
b p k
c q d
c r d
)");
            collector.clear();
            interactive.process("S is noted");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process("S s O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b s k"});
        }
        SUBCASE("in a collection, with a fresh variable")
        {
            process_lines(interactive, R"(
(X p Y) => (X likes @{((W r X, W s Y) => (c q d))}) (W t X)
e r f
e s g
a p b
)");
            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector).empty());
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1); });
}

TEST_CASE("derived rules: a rule with a fresh variable in a bucket gets one witness per binding")
{
    // The witness associated with W is a node no binding names. The term
    // of the bucket that the firing writes into stands in for the term's
    // node during the search for the witness, ensuring that the next pass
    // finds the witness and its membership and builds neither again. A
    // mention of such a rule is written once per binding, with its bucket
    // built by the construction.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const auto& [probe, query, answers] : std::vector<std::tuple<std::string, std::string, std::size_t>>{
                 {"(X p Y) => (W in @{Y})\na p b", "S in O", 1},
                 {"(G go H) => (((X G Y) => (Y in @{H})) noted yes)\nt go k\nu go m", "S noted O", 2},
                 {"(G go H) => (((X G Y) => (Y in @{k})) noted yes)\nt go k\nu go m", "S noted O", 2},
                 {"(G go H) => ((Y in @{H}) noted yes)\nt go k\nu go m", "S noted O", 1},
                 {"(G go H) => (((X G H) => (H in @{k})) noted yes)\nt go k\nu go m", "S noted O", 2}})
        {
            CAPTURE(probe);
            interactive.process(".new");
            const auto counts = counts_over_passes(collector, interactive, probe);
            CHECK(counts == std::vector<std::size_t>(4, counts.front()));
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).size() == answers);
        } });
}

TEST_CASE("derived rules: a container a switch rebuilds does not claim a ground member")
{
    // The construction builds the container for the rule a switch turns on
    // as one of its own, so each membership fact within that container
    // arises simultaneously with the rule, including the specific case of a
    // ground member. No element marked those facts as patterns: simply
    // activating the switch made `S in O` answer `k in @{k}`, and a rule
    // based on `in` derived `k flagged yes` from it. The typed rule
    // `(X p k) => (X likes {k X})` claims no membership.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
(K is on) => ((X p k) => (X likes {k X}))
k is on
)");
        interactive.run(true, false, false);

        // The switch wrote the rule.
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 3);

        collector.clear();
        interactive.process("S flagged O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("derived rules: a container a switch rebuilds does not claim an asserted member")
{
    // The same applies to an asserted fact regarding the member.
    // `k is green` stays a claim, and its membership in the rebuilt
    // container does not become one; the switch derived
    // `(k is green) flagged yes` from `(k is green) in @{(k is green)}`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
k is green
(K is on) => ((X r k) => (X likes {(k is green) X}))
k is on
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 3);

        collector.clear();
        interactive.process("S flagged O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("A is green");
        CHECK(answers_contain(collector, "k is green")); });
}

TEST_CASE("derived rules: a container in a generated rule takes the outer bindings next to the inner variables")
{
    // A container whose members still carry the INNER rule's variable
    // following substitution was returned unaltered, thus the generated rule
    // kept the generator's H:
    //
    //     (X p k) => (X likes @{(X q H) (H is green)})
    //
    // and `a p k` derived `a likes @{(X q H) (H is green)}`, including all
    // template variables. On the way, the member `k is green` was
    // constructed and then discarded, leaving it within the graph as a
    // claim that no one made. The typed rule
    // `(X p k) => (X likes {(X q k) (k is green)})` derives
    // `a likes {(a q k) (k is green)}` and claims nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((X p H) => (X likes {(H is green) (X q H)}))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is green");
        CHECK_FALSE(answers_contain(collector, "k is green"));

        collector.clear();
        interactive.process(".explain (k is green)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        // H remains exclusively within the
        // generator itself.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(count_outputs_containing(collector, "H is green") == 1);

        interactive.process("a p k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front().find("(a q k)") != std::string::npos);
        CHECK(answers.front().find("(k is green)") != std::string::npos);
        // Ground members constitute a set constant,
        // not a collection.
        CHECK(answers.front().find('@') == std::string::npos);

        // The container, once rebuilt, reverts to being the collection of
        // its recipe once more, meaning that a further run writes no second
        // rule.
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t rules_before = listed_rules(collector);

        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == rules_before); });
}

TEST_CASE("derived rules: a collection no binding changes and an equal one the generator rebuilds stay two")
{
    // `@{(Y q k)}` contains nothing the generator binds, and `@{(Y q H)}` is
    // rebuilt into a collection sharing identical members. Each forms the
    // collection of a recipe of its own template, thus the two remain
    // distinct; found by their members, one would coincide with the other,
    // and the generated rule would have a single object where the generator
    // writes two. The search executed by .explain must also maintain the
    // separation between the two. The answers correspond to those of the
    // typed `(X p k) => (X likes @{(Y q k)} @{(Y q k)})`.
    //
    // Both orders of the two within the
    // generator's text.
    const auto check = [](auto& collector, auto& interactive, const std::string& generator)
    {
        interactive.process(generator);
        interactive.process("now go k");
        interactive.process("b p k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("b likes S");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 2);
        for (const auto& answer : answers)
            CHECK(answer == "b likes @{(Y q k)}");

        collector.clear();
        interactive.process(".explain ((X p k) => (X likes @{(Y q k)} @{(Y q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (X likes @{(Y q k)} @{(Y q k)})
   └─ now go k  [axiom]
)");
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("the collection no binding changes written first")
        {
            check(collector, interactive, "(G go H) => ((X p H) => (X likes @{(Y q k)} @{(Y q H)}))");
        }
        SUBCASE("the rebuilt collection written first")
        {
            check(collector, interactive, "(G go H) => ((X p H) => (X likes @{(Y q H)} @{(Y q k)}))");
        } });
}

TEST_CASE("derived rules: a rebuilt subject collection and an equal object no binding changes stay two")
{
    // The same pair of collections serving as subject and object. Found for
    // the subject, the object would turn the consequence into the self-fact
    // `:likes @{(Y q k)}`, built with the rule and never marked: `S likes O`
    // would provide an answer before there is any data, and `b p k` would
    // derive nothing, because the consequence would already hold. The typed
    // `(X p k) => (@{(Y q k)} likes @{(Y q k)})` answers nothing before
    // `b p k` and its consequence afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((X p H) => (@{(Y q H)} likes @{(Y q k)}))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        interactive.process("b p k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front() == "@{(Y q k)} likes @{(Y q k)}"); });
}

TEST_CASE("derived rules: a generated generator keeps a collection no binding changes and an equal rebuilt one apart")
{
    // Moving one level higher: the outer rule writes the generator using
    // `@{(Y q H)} @{(Y q m)}`, and this generator rebuilds the first
    // collection into one equal to the second. Found for the first, the
    // second would leave the rule it writes with a single object, and
    // `b p m` would answer once.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A rel B) => ((G go H) => ((X p H) => (X likes @{(Y q H)} @{(Y q A)})))
m rel n
now go m
b p m
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("b likes S");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 2);
        for (const auto& answer : answers)
            CHECK(answer == "b likes @{(Y q m)}");

        collector.clear();
        interactive.process(".explain ((X p m) => (X likes @{(Y q m)} @{(Y q m)})) 0");
        CHECK(last_out_text(collector) == R"((X p m) => (X likes @{(Y q m)} @{(Y q m)})
   └─ now go m  [axiom]
)"); });
}

TEST_CASE("derived rules: two equal collections keep their places when a generated rule is derived again")
{
    // The subject's collection and the object's collection contain identical
    // members, with each being the collection of a recipe based on its
    // respective template. Derived again, the subject acquires its own
    // collection regardless of which one the store lists first; identified
    // through their members, it would get the second collection in roughly
    // one out of every four generators, and the rule with the two swapped
    // would be written as a new rule. Thirty-two generators ensure that the
    // order the store happens to produce cannot obscure it. Auto-run is
    // disabled during the counting of rules, so that a construction that
    // never stabilizes fails here instead.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run"); // a toggle: off
        for (int i = 0; i < 32; ++i)
            interactive.process("(G go H) => ((X p H) => (@{(Y q H)} likes" + std::to_string(i) + " @{(Y q H)}))");
        interactive.process("now go k");
        for (int pass = 0; pass < 3; ++pass)
            interactive.process(".run-once");

        // Each generator and a single rule
        // per generator.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 64); });
}

TEST_CASE("derived rules: a container rebuilt for a generated rule does not claim an existing atom as a member")
{
    // The container is rebuilt as a collection, meaning each membership
    // fact within it arises with the rule, including that of a member which
    // previously existed. A construction that solely marked membership
    // facts for members that are newly introduced would miss the case of k,
    // because an atom that predated `now go k` is not new: `S in O` would
    // answer that k belongs to the container, and a rule governing `in`
    // would derive `k flagged yes` from nothing but the rule being written.
    // The typed rule `(X p k) => (X likes {k X})` derives nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
(G go H) => ((X p H) => (X likes {H X}))
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("derived rules: a container rebuilt for a generated rule does not claim an asserted fact as a member")
{
    // The same applies to a fact concerning the member. Since `k is green`
    // was asserted before the generator fired, it stays a claim, and its
    // membership in the container built for the rule does not turn into
    // one: the typed rule `(X r k) => (X likes {(k is green) (X q k)})`
    // claims no membership either.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M in C) => (M flagged yes)
k is green
(G go H) => ((X r H) => (X likes {(H is green) (X q H)}))
now go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("A is green");
        CHECK(answers_contain(collector, "k is green")); });
}

TEST_CASE("derived rules: an ordinary rule rebuilds a container that keeps an unbound variable once per binding")
{
    // The rebuild in question applies to derived rules, and an ordinary rule
    // takes the identical route: Z is bound by no condition and gets no
    // witness within a container, thus the container continues to hold a
    // variable after substitution. Returned unchanged, as it used to be, it
    // served as the rule's own container: the bindings derived
    // `a likes @{Z Y}` and `c likes @{Z Y}`, sharing one container for both,
    // with the template variable Y in place of the value. Each binding now
    // possesses its own container that holds its member, and the next run
    // finds that container again through its members rather than
    // constructing a new one.
    //
    // The fate of Z is deliberately not asserted. It remains within
    // every container as the variable, while a witness -- what Z
    // acquires in `(X p Y) => (X has (Z q Y))` -- would be the correct
    // outcome.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes {Y Z})
a p b
c p d
)");
        interactive.run(true, false, false);

        // The members that are output for an answer's container, with a
        // space added on both sides to enable a member to be found via
        // " m ".
        const auto members = [](const std::string& answer)
        {
            const auto open  = answer.find('{');
            const auto close = answer.find('}', open);
            if (open == std::string::npos || close == std::string::npos) return std::string{};
            return " " + answer.substr(open + 1, close - open - 1) + " ";
        };

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 2);
        for (const auto& answer : answers)
        {
            const bool from_a = answer.rfind("a likes", 0) == 0;
            CHECK((from_a || answer.rfind("c likes", 0) == 0));
            CHECK(members(answer).find(from_a ? " b " : " d ") != std::string::npos);
            CHECK(members(answer).find(from_a ? " d " : " b ") == std::string::npos);
            CHECK(members(answer).find(" Y ") == std::string::npos);
        }

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a container an ordinary rule rebuilds prints the variable it keeps")
{
    // The container that `b p k` builds holds k and Z. A variable retreats
    // from a rule's own collection, which a firing might write into, yet
    // this collection is no part of a rule: printed as `@{k}`, the answer
    // would read as a collection excluding Z. The rule continues to output
    // its own container, listing the variables it names.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{Y Z})
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{k Z}"});

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p Y) => (X likes @{Z Y})"));

        // The membership fact involving Z within that collection employs a
        // variable as its subject, just as a pattern does, and still refers
        // to the whole collection.
        collector.clear();
        interactive.process(".node Z");
        CHECK(any_output_contains(collector, "- Z in @{k Z} (ID "));
        CHECK_FALSE(any_output_contains(collector, "- Z in @{Z} (ID ")); });
}

TEST_CASE("derived rules: a container a firing built keeps its variable when a generated rule names it")
{
    // The generator binds C to the collection constructed by `b p k`, hence
    // the rule it generates refers to that collection: a collection that a
    // firing built can flow into a rule via a binding. It remains the
    // identical node, and the rule does not claim Z as its own: Z occurs
    // nowhere else within the rule. If it were considered the rule's own
    // collection, it would output `@{Z}` within the rule and `@{k}` elsewhere,
    // the prior response `b likes @{k Z}` would shift under a rule unrelated
    // to `likes`, and the printed rule `(X q b) => (X r @{Z})` would re-enter
    // as a rule targeting a different collection.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{Y Z})
b p k
(A likes C) => ((X q A) => (X r C))
)");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((X q b) => (X r @{k Z})) ⇐ (b likes @{k Z})"));

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{k Z}"});

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X q b) => (X r @{k Z})"));

        collector.clear();
        interactive.process("d q b");
        interactive.run(true, false, false);
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"d r @{k Z}"});

        collector.clear();
        interactive.process("S in O");
        const auto answers = collect_answers(collector);
        CHECK(std::ranges::find(answers, "k in @{k Z}") != answers.end());

        collector.clear();
        interactive.process(".node Z");
        CHECK(any_output_contains(collector, "- Z in @{k Z} (ID "));
        CHECK_FALSE(any_output_contains(collector, "- Z in @{Z} (ID ")); });
}

TEST_CASE("derived rules: a container a firing built keeps its variable when a rule written later names the variable")
{
    // The two consequences stemming from a single statement share its
    // variables, meaning the rule that the second consequence writes names
    // the Z of the collection the first consequence builds, while also
    // holding that collection through C. Z is still not a variable of a
    // rule's own collection: the collection is the value that
    // `b likes @{k Z}` names. If treated as the rule's own collection, it
    // would print `@{Z}` within the rule and `@{k}` elsewhere starting from
    // the moment the rule is written, and the answer `b likes @{k Z}` would
    // shift accordingly.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{Y Z}) ((A likes C) => ((W q A) => (W r C Z X)))
b p k
)");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((W q b) => (W r b @{k Z} Z)) ⇐ (b likes @{k Z})"));

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{k Z}"});

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(W q b) => (W r b @{k Z} Z)"));

        collector.clear();
        interactive.process("S in O");
        const auto answers = collect_answers(collector);
        CHECK(std::ranges::find(answers, "k in @{k Z}") != answers.end());

        collector.clear();
        interactive.process(".node Z");
        CHECK(any_output_contains(collector, "- Z in @{k Z} (ID "));
        CHECK_FALSE(any_output_contains(collector, "- Z in @{Z} (ID ")); });
}

TEST_CASE("derived rules: printing a collection that a firing built reads none of the containers that hold it")
{
    // A collection a firing built is data, and its identifier indicates
    // this: printing it reads its own members and nothing above it.
    // Determined by the rules above it, the print operation ascended through
    // every container of which the collection is a member, and through every
    // item held within each such container: each of the 400 answers below
    // read the 200,000 members of `bucket`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions off
(X p Y) => (X likes @{Y Z})
(X likes C) => (C in bucket)
%(for i 0 200000 (zelph/fact (string "m" i) "in" "bucket"))
%(for i 0 400 (zelph/fact (string "a" i) "p" "k"))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        CHECK(answers.size() == 400);
        CHECK(std::ranges::all_of(answers, [](const std::string& answer)
                                  { return answer.ends_with(" likes @{k Z}"); })); });
}

// A collection constructed in the argument forms of zelph/rule is the rule's
// own, just as a typed rule's literal is, and each firing writes into its
// term. The rule continues to print its own collection using the variables it
// names, and an answer prints the term along with its accumulated contents.
// Printed with each member the term holds, the accumulator would list as
// `(Y in @{bug1 bug2 Y X})`, which re-enters as a rule whose collection
// initially holds two bug reports, and each answer would name the rule's
// variables as members.
TEST_CASE("derived rules: an accumulator built through zelph/rule prints what it has accumulated")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(zelph/rule [(zelph/fact 'X "reported" 'Y)] (zelph/fact 'Y "in" (zelph/collection 'X))))");
        interactive.process("alice reported bug1");
        interactive.process("bob reported bug2");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "=> (Y in @{Y X})"));
        CHECK_FALSE(any_output_contains(collector, "bug"));

        collector.clear();
        interactive.process("S in O");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"bug1 in @{bug1 bug2}", "bug2 in @{bug1 bug2}"});

        // A rule concerning `in` derives a fact regarding the collection
        // based on the membership facts from the bug reports, with the fact
        // naming the term, a value. The rule's own collection remains
        // unaltered.
        interactive.process("(M in K) => (K seen yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "=> (Y in @{Y X})"));
        CHECK_FALSE(any_output_contains(collector, "bug"));

        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"@{bug1 bug2} seen yes"}); });
}

TEST_CASE("derived rules: a collection of a rule built through zelph/rule keeps its variables out of an answer")
{
    // The consequence designates the rule's own `@{a Y}`, built in the
    // argument forms, meaning its membership facts are rule patterns, and
    // `S in O` answers neither that of `a` nor that of Y. The collection is
    // not an accumulator: the rule never performs writes into it, and a
    // firing rebuilds it for `b p k` as the set constant `{a k}`, with its
    // memberships constituting the essence of the set.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "likes" (zelph/collection "a" 'Y))))");
        interactive.process("b p k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes {a k}"});

        collector.clear();
        interactive.process("S in O");
        const auto answers = collect_answers(collector);
        CHECK(std::ranges::find(answers, "a in {a k}") != answers.end());
        CHECK(std::ranges::find(answers, "k in {a k}") != answers.end());
        CHECK(answers.size() == 2);
        for (const auto& answer : answers)
            CHECK(answer.find('Y') == std::string::npos); });
}

TEST_CASE("derived rules: an ordinary rule keeps two equal containers of its consequence apart")
{
    // The consequence names two containers, and `b p k` rebuilds both using
    // identical members. Looked up solely by their members, the second
    // would coincide with the first, causing the rule to derive
    // `b likes @{(Z q k)}` with a single object while naming two. The
    // construction of a rule ensures these containers remain distinct
    // (refer to "a collection no binding changes and an equal one the
    // generator rebuilds stay two"), and a firing must maintain this
    // separation. The subsequent run locates each container once more
    // rather than building a new one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{(Z q Y)} @{(Z q Y)})
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("b likes S");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 2);
        for (const auto& answer : answers)
            CHECK(answer == "b likes @{(Z q k)}");

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: an ordinary rule keeps a rebuilt container apart from an equal one it names")
{
    // `b p k` rebuilds the subject `@{(Z q Y)}` into a container equal to the
    // object `@{(Z q k)}`, which the rule names and no binding changes. Found
    // for the subject, the object would make the consequence the self-fact
    // `:likes @{(Z q k)}`. The typed `(X p k) => (@{(Z q k)} likes @{(Z q k)})`
    // derives a fact connecting two containers, just as this rule does.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (@{(Z q Y)} likes @{(Z q k)})
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front() == "@{(Z q k)} likes @{(Z q k)}");

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a generated rule keeps a rebuilt container apart from an equal one it names")
{
    // The identical rule, written by a generator. Its construction ensures the
    // two containers remain separate, and `now go k` writes
    // `(X p Y) => (@{(Z q Y)} likes @{(Z q k)})`. Firing that rule for `b p k`
    // must likewise maintain their separation, as previously described.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p Y) => (@{(Z q Y)} likes @{(Z q H)}))
now go k
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front() == "@{(Z q k)} likes @{(Z q k)}");

        rerun_adds_no_node(collector, interactive); });
}

// The rules a generator writes share the generator's variables, meaning a
// container created through firing and a container created through
// construction may possess identical members. A lookup based solely on
// members finds one container for the other across the line between data and
// rules: the derived fact then refers to the precise container that a
// generated condition names, and the condition, matching by identity, holds.
// The generated rules, typed as they are printed, consist of two statements
// each containing a Z, and derive nothing of that kind.
TEST_CASE("derived rules: a firing does not take a container of a generated condition")
{
    // `now go k` writes `((X p @{(Z q k)}), (X r Y)) => (X likes Y)`
    // initially. Looked up by its members alone, spanning the boundary
    // separating rules from data, the firing of the other rule for `b s k`
    // finds that condition container, and `b r m` derives `b likes m`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X s Y) => (X p @{(Z q Y)})) ((X p @{(Z q H)}, X r Y) => (X likes Y))
now go k
b s k
b r m
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b p @{(Z q k)}"});

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a firing does not take a container that the generator names")
{
    // The condition container for the second rule is the construction's own
    // collection associated with the generator's `@{(Z q k)}`, which holds
    // nothing that the generator binds. The firing of the first rule for
    // `b s k` builds a term having identical members; identified through its
    // members, the construction's collection would be that term, and
    // `b likes k` would follow.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X s Y) => (X p @{(Z q Y)})) ((X p @{(Z q k)}) => (X likes H))
now go k
b s k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b p @{(Z q k)}"});

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a construction does not take a container that a firing built")
{
    // In the opposite direction: the switch writes the rule containing the
    // condition only once `b s k` has built `@{(Z q k)}` as data, and a
    // construction that searched for the condition container using its
    // members alone would find that one. The .explain search still finds the
    // container the construction built, even though the data container
    // shares the same members.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X s Y) => (X p @{(Z q Y)})) ((K on L) => (((X p @{(Z q L)}), (X r Y)) => (X likes Y)))
now go k
b s k
b r m
sw on k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        // The order of the conditions adheres to the node ids, meaning
        // that only the premise line is compared exactly.
        collector.clear();
        interactive.process(".explain (((X r Y), (X p @{(Z q k)})) => (X likes Y)) 0");
        const std::string tree = last_out_text(collector);
        CHECK(tree.find("=> (X likes Y)\n   └─ sw on k  [axiom]\n") != std::string::npos);
        CHECK(tree.find("[axiom]") == tree.rfind("[axiom]"));

        rerun_adds_no_node(collector, interactive); });
}

// The same sharing applies on the rules’ side: a single firing of a generator
// builds each rule it writes within a distinct construction, and the
// containers of one rule can contain precisely the members of the containers
// of another. A construction that isolates only its own rule's containers
// takes, for the second rule, the collection that the first construction
// produced for a different container of the generator, or the generator's own
// collection that represents a container of another rule. The rules then
// share a term that the generator writes twice, and derive facts that the
// same rules, when typed as they appear in print, do not derive. A second
// trigger involving a binding the generator has previously encountered must
// locate each rule once more with the collections it was originally written
// with; a firing that took them in a different sequence would write the rules
// anew.
TEST_CASE("derived rules: two rules of one firing do not share a rebuilt collection")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)})) ((X p H) => (X hates @{(Z q H)}))
(X likes C, X hates C) => (X torn C)
now go k
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});

        collector.clear();
        interactive.process("S hates O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b hates @{(Z q k)}"});

        // The search of .explain meets the rules in the sequence in
        // which the firing built them, and identifies each with its own
        // collection.
        collector.clear();
        interactive.process(".explain ((X p k) => (X likes @{(Z q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (X likes @{(Z q k)})
   └─ now go k  [axiom]
)");
        collector.clear();
        interactive.process(".explain ((X p k) => (X hates @{(Z q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (X hates @{(Z q k)})
   └─ now go k  [axiom]
)");

        trigger_adds_no_rule(collector, interactive, "then go k");

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a construction does not take the generator's collection of another rule")
{
    // The second rule names the generator's `@{(Z q k)}`, which holds
    // nothing that the generator binds, and its construction builds it as a
    // collection of its own. The construction of the first rule rebuilds
    // `@{(Z q H)}` into a collection having identical members, and a lookup
    // based solely on those members would yield the same collection for
    // both rules.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)})) ((X r H) => (X hates @{(Z q k)}))
(X likes C, X hates C) => (X torn C)
now go k
b p k
b r k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        trigger_adds_no_rule(collector, interactive, "then go k");

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a condition of one generated rule does not share a collection with the consequence of another")
{
    // The first rule derives `b likes @{(Z q k)}` from `b p m`. With
    // its collection shared with the condition of the second rule, that
    // condition holds, and `b happy k` follows.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p Y) => (X likes @{(Z q H)})) ((X likes @{(Z q H)}) => (X happy H))
now go k
b p m
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S happy O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});

        trigger_adds_no_rule(collector, interactive, "then go k");

        collector.clear();
        interactive.process("S happy O");
        CHECK(collect_answers(collector).empty());

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a generated generator does not share a collection with a rule of the same firing")
{
    // The generator writes a rule and a generator, each having a
    // collection rebuilt from an `@{(Z q H)}` specific to itself. With
    // one collection shared between them, the condition of every rule
    // that the second generator writes -- both for `now go k` and for
    // `then go m` -- matches the fact `b likes @{(Z q k)}` that the first
    // rule derives from `b p k`, and `b happy k` and `b happy m` result
    // accordingly.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)})) ((I go J) => ((X likes @{(Z q H)}) => (X happy J)))
now go k
then go m
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S happy O");
        CHECK(collect_answers(collector).empty());

        trigger_adds_no_rule(collector, interactive, "again go k");

        collector.clear();
        interactive.process("S happy O");
        CHECK(collect_answers(collector).empty());

        rerun_adds_no_node(collector, interactive); });
}

// The firings of a single generator across distinct bindings write distinct
// rules, and each rule they write has collections of its own, just as the
// identical rules, typed as they are printed, do: a construction builds each
// collection under the recipe of the generator's container and the binding
// associated with the rule it writes. A construction provided with an
// existing collection possessing the right members would give the rules from
// two firings one collection wherever their containers have matching
// members, regardless of whether the rules come from a single container of
// the generator or from two separate ones, and they would derive facts
// concerning that single collection which the typed rules do not derive.
TEST_CASE("derived rules: the firings of a generator for two bindings do not share a collection")
{
    // `likes go k` and `hates go k` write `(X p k) => (X likes @{(Z q k)})`
    // and `(X p k) => (X hates @{(Z q k)})`. Given the collection that the
    // first firing built, which has the same members, the second rule would
    // share it with the first, and `b p k` would derive `b torn @{(Z q k)}`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X G @{(Z q H)}))
(X likes C, X hates C) => (X torn C)
likes go k
hates go k
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
        collector.clear();
        interactive.process("S hates O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b hates @{(Z q k)}"});

        // The search of .explain finds the rule associated with the
        // second firing, with its own collection.
        collector.clear();
        interactive.process(".explain ((X p k) => (X hates @{(Z q k)})) 0");
        CHECK(last_out_text(collector) == R"((X p k) => (X hates @{(Z q k)})
   └─ hates go k  [axiom]
)");

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: the firings of a generator do not share a collection that no binding changes")
{
    // `(G go H) => ((X p H) => (X G @{(Z q k)}))` writes
    // `(X p k) => (X likes @{(Z q k)})` for `likes go k` and
    // `(X p k) => (X hates @{(Z q k)})` for `hates go k`. The generator does
    // not bind anything within the collection. If returned unchanged, the
    // generator's own collection would be held by both rules, leading to
    // `b p k` deriving `b torn @{(Z q k)}`, a result that the two rules,
    // typed as they are printed, do not derive. The same holds one level
    // deeper, for the rules written by a generated generator.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& lines, const std::string& binding)
        {
            interactive.process("(X likes C, X hates C) => (X torn C)");
            process_lines(interactive, lines);
            interactive.process("b p " + binding);
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S torn O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
            collector.clear();
            interactive.process("S hates O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b hates @{(Z q k)}"});

            rerun_adds_no_node(collector, interactive);
        };

        SUBCASE("a generator")
        {
            check("(G go H) => ((X p H) => (X G @{(Z q k)}))\nlikes go k\nhates go k", "k");

            collector.clear();
            interactive.process(".explain ((X p k) => (X hates @{(Z q k)})) 0");
            CHECK(last_out_text(collector) == R"((X p k) => (X hates @{(Z q k)})
   └─ hates go k  [axiom]
)");
        }
        SUBCASE("a generated generator")
        {
            check("(G go H) => ((I go J) => ((X p J) => (X I @{(Z q k)})))\nnow go k\nlikes go m\nhates go m", "m");
        } });
}

TEST_CASE("derived rules: a firing that found one of its rules builds a collection no binding changes for another")
{
    // The firings for `likes go k` and `hates go k` find the rule concerning
    // `keep` that `warm go k` wrote, and build the rule for `likes`, followed
    // by the one for `hates`, anew. The generator's `@{(Z q k)}` in each case
    // constitutes the collection of a recipe keyed by that rule's binding of
    // G. If taken as-is, this collection would be held by both rules, and
    // `b s k` would derive `b torn @{(Z q k)}`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X keep @{(Z q k)})) ((X s H) => (X G @{(Z q k)}))
(X likes C, X hates C) => (X torn C)
warm go k
likes go k
hates go k
b s k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        // The generator, the rule governing `torn`, the rule governing
        // `keep`, and a rule over `s` for each binding.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 6);

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a later firing does not take a collection that a rule of an earlier firing holds")
{
    // The rules concerning c2 and c3 come from two containers within the
    // generator that possess identical members. Removing the collection
    // associated with the rule for c2 removes that rule, while removing
    // `now go k` keeps the generator from firing for `now` before `then`. The
    // firing for `then go k` writes the rule for c2 anew, over the collection
    // defined by its recipe. Given an existing collection with the right
    // members, it would take the one held by the rule for c3 of the first
    // firing, and `b torn @{(Z q k)}` would then follow. Re-entered,
    // `now go k` would subsequently locate the rule for c2 over that
    // collection, and write the rule for c3 a second time.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X c2 H) => (X r2 @{(Z q H)})) ((X c3 H) => (X r3 G @{(Z q H)}))
(X r2 C, X r3 G C) => (X torn C)
now go k
)");
        remove_collection_in(collector, interactive, "r2", "X r2 @{(Z q k)}");
        remove_listed(collector, interactive, "now", "now go k");
        process_lines(interactive, R"(
then go k
b c2 k
b c3 k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S r2 O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b r2 @{(Z q k)}"});

        trigger_adds_no_rule(collector, interactive, "now go k");

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a rule of an earlier firing keeps its collection to itself across .save and .load")
{
    // The collection of the rule over c2 is removed as shown previously, yet
    // `now go k` remains intact, and the network goes through .save, .new,
    // and .load. The classic evaluation following the load fires the
    // generator for `then go k` before firing it for `now go k`, an order
    // that the semi-naive approach does not replicate. Assuming a
    // pre-existing collection containing the right members, the firing
    // associated with `then` would take the collection of the rule over c3
    // for the rule over c2, `b torn @{(Z q k)}` would then follow, and the
    // firing linked to `now` would write the rule over c3 once more. The ids
    // that the load restores are the recipes that the firings recompute.
    const auto file = fs::temp_directory_path() / "zelph_derived_rules_earlier_firing_test.bin";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X c2 H) => (X r2 @{(Z q H)})) ((X c3 H) => (X r3 G @{(Z q H)}))
(X r2 C, X r3 G C) => (X torn C)
now go k
)");
        remove_collection_in(collector, interactive, "r2", "X r2 @{(Z q k)}");
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");

        // The .load command causes
        // auto-run to deactivate.
        process_lines(interactive, R"(
.semi-naive off
then go k
b c2 k
b c3 k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        // The generator, the rule governing `torn`, the rule governing c2,
        // which is the same rule for both bindings, and a rule governing c3
        // for each binding.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 5);

        rerun_adds_no_node(collector, interactive); });

    fs::remove(file);
}

TEST_CASE("derived rules: a construction does not take a collection that a binding puts into the rule")
{
    // `b r now @{(Z q k)}` feeds the collection associated with the rule that
    // `now go k` writes back into the generator as G. The rule written for
    // that binding names the collection as the value of G and rebuilds
    // `@{(Z q H)}` adjacent to it. Given the collection of G for the rebuilt
    // container, the rule would hold a single object where the generator
    // writes two, and print as `(X p k) => (X r @{(Z q k)})`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X r G @{(Z q H)}))
(X r now C) => (C go k)
now go k
b p k
)");
        interactive.run(true, false, false);

        // The generator, the rule concerning `r`, and the rules for `now`
        // and for the collection.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p k) => (X r @{(Z q k)} @{(Z q k)})"));
        CHECK(listed_rules(collector) == 4);

        rerun_adds_no_node(collector, interactive); });
}

// The collections of a rule a firing writes are those of their recipes:
// written again, the rule remains the identical node over the same
// collections, and no alternative rule's collection replaces them. The tests
// that follow eliminate rules and collections, so that an existing collection
// with the right members belongs to a different rule than the one written, or
// to none at all. A firing that took such a collection for the rule would
// count the rule as missing and write it again, and if it built the rule over
// that collection, two rules would end up sharing it.
TEST_CASE("derived rules: a rule written again after its collection was removed is the same node over the same collection")
{
    // The collection that a construction builds carries the identifier of its
    // recipe: the generator's container and the binding of the rule written.
    // The rule over it is a fact, computed by hashing its components. When
    // the collection is removed, the rule is removed alongside it, and the
    // next firing writes both again under the same identifiers they
    // previously held, meaning an id read off `.node` before the removal
    // continues to refer to the same rule afterwards. `then go k` binds G to
    // a different value, which the rule does not hold: its recipe is the one
    // provided by `now go k`. If a collection identifier were obtained from a
    // counter, or from a key that holds anything else, the rule would
    // reappear as a fresh node over a new collection, and `b p k` would
    // derive a second `b likes @{(Z q k)}` through it, over a term of its
    // own.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)}))
now go k
b p k
)");
        interactive.run(true, false, false);

        // The rule and its collection, identified via the consequence of the
        // rule.
        const auto listed = [&](const std::string& label)
        {
            collector.clear();
            interactive.process(".node likes");
            const std::string consequence = listed_id(collector, "X likes @{(Z q k)}");
            REQUIRE_FALSE(consequence.empty());
            collector.clear();
            interactive.process(".node " + consequence);
            const std::string id = listed_id(collector, label);
            REQUIRE_FALSE(id.empty());
            return id;
        };
        const std::string rule       = listed("(X p k) => (X likes @{(Z q k)})");
        const std::string collection = listed("@{(Z q k)}");

        interactive.process(".remove " + collection);

        // The rule was taken along with the removal: only the generator
        // remains.
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        interactive.process("then go k");
        interactive.run(true, false, false);

        CHECK(listed("(X p k) => (X likes @{(Z q k)})") == rule);
        CHECK(listed("@{(Z q k)}") == collection);

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a rule written again after its collection was removed keeps apart from the other rules of the firing")
{
    // Removing the collection of the rule the firing writes first removes
    // that rule along with it, leaving behind a collection with its members,
    // which belongs to the other rule. Upon the generator firing once more,
    // if it were taken for the first rule, it would be shared as in "two
    // rules of one firing do not share a rebuilt collection":
    // `b torn @{(Z q k)}` would then follow, and the other rule would be
    // written again over a distinct collection of its own.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)})) ((X p H) => (X hates @{(Z q H)}))
(X likes C, X hates C) => (X torn C)
)");
        collector.clear();
        interactive.process("now go k");
        const std::vector<std::string> order = announced_order(collector, {"likes", "hates"});
        interactive.process("b p k");
        interactive.run(true, false, false);

        remove_collection_of(collector, interactive, order[0]);
        interactive.process("then go k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
        collector.clear();
        interactive.process("S hates O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b hates @{(Z q k)}"});

        // The generator, the rule over `torn`, and the two rules it
        // writes.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 4);

        trigger_adds_no_rule(collector, interactive, "again go k");
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a rule found among others is found again when another rule of the firing is written anew")
{
    // Two removals occur, following the order in which the firing writes the
    // rules: once the first is executed, the second rule is written anew,
    // over the collection of its recipe. Upon completion of the second, the
    // first rule is missing, and the collections containing the right
    // members belong to the second and third rules. If taken as the first
    // rule written anew, the second rule's collection would be shared, and
    // the second rule would be considered missing, requiring it to be
    // written once more.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A single rule involving two predicate variables identifies any pair
        // among the three that have a shared collection, naming none of them:
        // `.node <predicate>` lists the rule each one is written in only when
        // each has a small number of neighbours.
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)})) ((X p H) => (X hates @{(Z q H)})) ((X p H) => (X loves @{(Z q H)}))
(X P C, X Q C, P != Q) => (X torn C)
)");
        collector.clear();
        interactive.process("now go k");
        const std::vector<std::string> order = announced_order(collector, {"likes", "hates", "loves"});
        interactive.process("b p k");
        interactive.run(true, false, false);

        remove_collection_of(collector, interactive, order[1]);
        interactive.process("then go k");
        interactive.run(true, false, false);
        remove_collection_of(collector, interactive, order[0]);
        interactive.process("again go k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        for (const std::string predicate : {"likes", "hates", "loves"})
        {
            collector.clear();
            interactive.process("S " + predicate + " O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b " + predicate + " @{(Z q k)}"});
        }

        // The generator, the rule concerning `torn`, and the three rules it
        // writes.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 5);

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a rule is found again behind any number of collections left by removed rules")
{
    // Taking away the condition from a generated rule in turn eliminates the
    // rule and leaves its collection, which includes the members from the
    // collections of the rules adjacent to it. In this case, the first 64
    // rules the firing writes lose their conditions, and the 65th rule loses
    // its collection. When the generator fires once more, the 64 collections
    // left behind have the members of the 66th rule's own collection, which
    // persists. Every rule written anew is built over the collection defined
    // by its recipe, regardless of whatever else holds its members: a firing
    // that scanned collections with the right members, and abandoned the
    // search after a fixed number of tries, would count the 66th as missing,
    // and the 65th, which the firing writes anew before it, would take the
    // collection the search had not reached: the two rules would share it,
    // `b torn @{(Z q k)}` would follow, and the 66th would be written again.
    constexpr int rules = 66;

    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string              generator = "(G go H) =>";
        std::vector<std::string> predicates;
        for (int i = 1; i <= rules; ++i)
        {
            const std::string n = std::to_string(i);
            generator += " ((X c" + n + " H) => (X r" + n + " @{(Z q H)}))";
            predicates.push_back("r" + n);
        }
        interactive.process(generator);

        collector.clear();
        interactive.process("now go k");
        const std::vector<std::string> order = announced_order(collector, predicates);

        for (int i = 0; i < rules - 2; ++i)
        {
            const std::string n = order[i].substr(1);
            collector.clear();
            interactive.process(".node c" + n);
            const std::string condition = listed_id(collector, "X c" + n + " k");
            REQUIRE_FALSE(condition.empty());
            interactive.process(".remove " + condition);
        }

        const std::string x = order[rules - 2].substr(1);
        const std::string y = order[rules - 1].substr(1);
        remove_collection_in(collector, interactive, "r" + x, "X r" + x + " @{(Z q k)}");

        interactive.process("(X r" + x + " C, X r" + y + " C) => (X torn C)");
        interactive.process("then go k");
        interactive.process("b c" + x + " k");
        interactive.process("b c" + y + " k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("b r" + y + " O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b r" + y + " @{(Z q k)}"});

        // The generator, the rule over `torn`, and the rules it
        // writes.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == static_cast<std::size_t>(rules + 2));

        trigger_adds_no_rule(collector, interactive, "again go k");
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a rule is found again when a collection it holds as a value comes first")
{
    // The rule over c1 feeds its own collection back: `b c1 k` derives
    // `@{(Z q k)} go k` over it, and the generator fires once more with G
    // bound to that collection. The rule over c2 that this firing writes
    // holds the collection as the value of G, and a newer one with identical
    // members where it rebuilds `@{(Z q H)}`.
    //
    // Removed then: `now go k` and `b c1 k`, ensuring the generator fires
    // solely when the collection is fed back and the rule over c1 does not
    // fire again, and the rule over c1, via its condition. The rule over c2
    // that the firing writes holds the value of G and the collection of its
    // own recipe beside it; identified through their members, one would be
    // taken for the other, the rule would be deemed missing and written a
    // second time over a new collection, on every pass. Auto-run is off
    // during the counting of passes, so that such an engine fails the
    // REQUIRE instead.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X c1 H) => (@{(Z q H)} go H)) ((X c2 H) => (X r2 G @{(Z q H)}))
now go k
b c1 k
)");
        interactive.run(true, false, false);

        // The rule governing c1 and, for each firing, a rule governing c2,
        // along with the generator.
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 4);

        interactive.process(".auto-run"); // a toggle: off
        remove_listed(collector, interactive, "now", "now go k");
        remove_listed(collector, interactive, "c1", "b c1 k");
        remove_listed(collector, interactive, "c1", "X c1 k");
        for (int pass = 0; pass < 3; ++pass)
            interactive.process(".run-once");

        // The generator, the rule over c1, written anew, and the two rules
        // over c2.
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 4);

        interactive.process(".auto-run"); // on again
        rerun_adds_no_node(collector, interactive); });
}

namespace
{
    // Enters `lines` with auto-run disabled, executes three single passes,
    // and requires `rules` rules afterwards: a firing that fails to locate a
    // rule it has written writes it again during each pass, and counted
    // passes fail the REQUIRE where a run would not end. Auto-run is
    // reactivated afterwards.
    void passes_keep_rules(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& lines, const std::size_t rules)
    {
        interactive.process(".auto-run"); // a toggle: off
        process_lines(interactive, lines);
        for (int pass = 0; pass < 3; ++pass)
            interactive.process(".run-once");

        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == rules);

        interactive.process(".auto-run"); // on again
    }
}

// A rule whose conditions do not involve any variable outside its
// collections: its conjunction set is a collection that the construction
// builds under its recipe, rediscovered through its members during the next
// pass, just as the rule's other collections are retrieved via their
// respective recipes. If considered as anything different, the rule would be
// deemed missing, and each pass would write it anew over a collection of its
// own.
TEST_CASE("derived rules: a rule whose conditions hold variables only in collections is found again")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& lines, const std::string& rule, const std::string& premise)
        {
            // The generator or the rule that performs writing, and the
            // rule that has been written.
            interactive.process(".semi-naive on");
            passes_keep_rules(collector, interactive, lines, 2);

            // The order of the conditions adheres to the node ids, meaning
            // only the premise line is compared exactly.
            collector.clear();
            interactive.process(".explain " + rule + " 0");
            const std::string tree = last_out_text(collector);
            CHECK(tree.find("\n   └─ " + premise + "  [axiom]\n") != std::string::npos);
            CHECK(tree.find("[axiom]") == tree.rfind("[axiom]"));

            rerun_adds_no_node(collector, interactive);
            interactive.process(".semi-naive check");
            rerun_adds_no_node(collector, interactive);
        };

        SUBCASE("the variable in a collection of the consequence")
        {
            check("(X p Y) => (((X s k), (X r Y)) => (X likes @{(Z q Y)}))\nb p m", "(((b s k), (b r m)) => (b likes @{(Z q m)}))", "b p m");
        }
        SUBCASE("the variable in a collection of a condition")
        {
            check("(G go H) => (((k q @{(Z r H)}), (k s c)) => (k t c))\nnow go m", "(((k q @{(Z r m)}), (k s c)) => (k t c))", "now go m");
        }
        SUBCASE("a negated condition beside it")
        {
            check("(X p Y) => (((X q @{(Z r Y)}), ¬(X s k)) => !)\nb p m", "(((b q @{(Z r m)}), ¬(b s k)) => !)", "b p m");
        } });
}

TEST_CASE("derived rules: a rule over an atom that is a member of itself is found again")
{
    // `k in k` establishes k as a container having k as its member.
    // `instantiate_fact` meets k once more beneath k, leaving it unchanged,
    // and the rule's prediction (ground_instance) must likewise leave it as
    // it is. If k were taken for a part it could not predict, the rule would
    // remain undiscovered, and each pass would rewrite it anew over a
    // collection of its own.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        passes_keep_rules(collector, interactive, "k in k\n(G go H) => ((X p H) => (X G k) (X w @{(Z q H)}))\nt go m", 2);

        interactive.process(".semi-naive check");
        rerun_adds_no_node(collector, interactive); });
}

// A rule over `in` writes into what a firing derived:
// `(X likes C) => (X in C)` puts `b` into the term built by the firing of
// `(X p k) => (X likes @{(Z q k)})`. The rule's own collection receives
// nothing, and the generator that wrote the rule finds it over the
// collection of its recipe regardless of what was written into the term.
// Found by the members of the collection the fact names, the rule would be
// deemed missing once a firing has written into it, and each subsequent
// firing of the generator would write it again: under `.semi-naive on` on
// every later input line, whether connected or not, and during a classic run
// on every pass, so that the run would not end.
TEST_CASE("derived rules: a generated rule is found again after a rule wrote into its collection")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto check = [&](const std::string& lines)
        {
            interactive.process(".semi-naive on");
            process_lines(interactive, lines);
            interactive.process("now go k");
            interactive.process("b p k");
            trigger_adds_no_rule(collector, interactive, "u v w");
            rerun_adds_no_node(collector, interactive);

            // The generator, the rule pertaining to `in`, and the
            // rule that has been written.
            interactive.process(".semi-naive off");
            passes_keep_rules(collector, interactive, "", 3);

            interactive.process(".semi-naive check");
            rerun_adds_no_node(collector, interactive);

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).size() == 1);
        };

        SUBCASE("a member that a firing writes")
        {
            check("(G go H) => ((X p H) => (X likes @{(Z q H)}))\n(X likes C) => (X in C)");

            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b in @{b (Z q k)}"});
        }
        SUBCASE("a member that the construction gave and a firing derives")
        {
            check("(G go H) => ((X p H) => (X likes @{H Z}))\n(X likes C) => (k in C)");
        } });
}

// A saved network keeps the rules a firing authored alongside their
// collections. Upon .load, a trigger possessing a binding the generator has
// encountered locates both rules once more, each paired with its respective
// collection: it introduces neither a new rule nor a new node, and the rules
// answer as their typed twins do. Every rule is located over the collections
// of its recipes regardless of the order in which the firing meets the rules,
// thus this behaviour does not rest on the load keeping the order of the
// generator's consequences.
TEST_CASE("derived rules: the rules of one firing are found again after .save and .load")
{
    const auto file = fs::temp_directory_path() / "zelph_derived_rules_firing_order_test.bin";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X likes @{(Z q H)})) ((X p H) => (X hates @{(Z q H)}))
(X likes C, X hates C) => (X torn C)
now go k
)");
        interactive.run(true, false, false);
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");

        // `.load` turns auto-run off, which causes the trigger to wait for
        // the reasoning below, so the tally taken before that reasoning
        // includes the additions the trigger made to the graph.
        interactive.process("then go k");
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t rules = listed_rules(collector);
        CHECK(rules == 4);
        collector.clear();
        interactive.process(".stat");
        const std::string nodes = node_count(collector);
        REQUIRE_FALSE(nodes.empty());

        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == rules);
        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == nodes);

        interactive.process("b p k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S torn O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
        collector.clear();
        interactive.process("S hates O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b hates @{(Z q k)}"}); });

    fs::remove(file);
}

// ---------------------------------------------------------------------------
// A fresh variable located next to a container that the bindings
// rebuild.
//
// Before a rule involving a fresh variable fires, consequences_already_exist
// determines if any existing nodes for that variable already make every
// consequence a fact (logic.md, "No duplicate witnesses"). Upon firing, a
// container that takes a binding is rebuilt; thus, the fact derived during
// the first firing names the rebuilt container. A check comparing the rule's
// own container to this one via identity never finds the witness, and each
// firing generates an additional `??` and a new fact: under check mode, such
// a run never concludes, and under `.semi-naive on`, each further input line
// fires the rule once again. The tests execute with `.semi-naive on`,
// ensuring that an engine lacking the prediction fails them rather than
// proceeding. The typed twins, whose containers remain unaffected by any
// binding, produce a single witness and then cease.
// ---------------------------------------------------------------------------

TEST_CASE("derived rules: a fresh variable beside a rebuilt container gets one witness")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive on
(X p Y) => (X likes @{(V s Y)} W)
b p k
u v w
)");

        collector.clear();
        interactive.process("b likes S");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"b likes ??", "b likes @{(V s k)}"});

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a fresh subject of a rebuilt container gets one witness")
{
    // The subject stands as the sole fresh segment, hence the container
    // serves as the exclusive node from which the check can initiate.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive on
(X p Y) => (W likes @{(V s Y)})
b p k
u v w
)");

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"?? likes @{(V s k)}"});

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a fresh variable in a rule term over a rebuilt container gets one witness")
{
    // The container resides within a rule that the consequence holds as a
    // term, positioned two levels beneath the fact, and W is fresh because
    // the outer rule does not read the inner one as a rule. Absent the
    // prediction, this configuration fails to complete under
    // `.semi-naive on` as well, thus auto-run is off while three passes run
    // one at a time, and the answer constitutes a REQUIRE before the full
    // run below.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive on
.auto-run
(X p Y) => (X says ((W q Y) => (W r @{(V s Y)})))
b p k
.run-once
.run-once
.run-once
)");

        collector.clear();
        interactive.process("b says S");
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"b says ((?? q k) => (?? r @{(V s k)}))"});

        interactive.process(".auto-run"); // on again
        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a consequence over a rebuilt container is a witness beside a fresh one")
{
    // The first consequence involves no fresh variable and is looked up as
    // the fact that the firing would derive, over the term the firing
    // constructs. Looked up using the rule's own container, it would be
    // missing, and no witness for W could help.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive on
(X p Y) => (X likes @{(V s Y)}) (X has W)
b p k
u v w
)");

        collector.clear();
        interactive.process("b has S");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b has ??"});

        collector.clear();
        interactive.process("b likes S");
        CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(V s k)}"});

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: a fresh variable beside a rebuilt set constant gets one witness")
{
    // `{Y a}` constitutes a collection within the rule, as Y functions as a
    // variable, and the firing rebuilds it into the set constant `{k a}`. A
    // check that compared the rule's own container through identity would
    // never meet `{k a}`, and each firing would generate a distinct
    // witness.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive on
(X p Y) => (X likes {Y a} W)
b p k
u v w
)");

        collector.clear();
        interactive.process("b likes S");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        REQUIRE(answers.size() == 2);
        CHECK(answers[0] == "b likes ??");
        // The order of the members follows the node ids.
        CHECK((answers[1] == "b likes {a k}" || answers[1] == "b likes {k a}"));

        rerun_adds_no_node(collector, interactive); });
}

TEST_CASE("derived rules: the premise may go, and the rule it wrote stays")
{
    // A switch invites the expectation that turning it OFF turns the rule off
    // again. It does not, and that is the general model rather than a gap in
    // this feature: zelph does no truth maintenance, so a derived FACT
    // outlives its premise, and a derived RULE is a derived fact. What
    // removes one is `.remove <id>` -- after which the generator writes it
    // again on the next run, because the premise is back to standing.
    //
    // Pinned here so that a future reader knows the asymmetry is intended:
    // the switch decides when the rule COMES INTO BEING, not how long it
    // lives.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
k is on
a p b
(K is on) => ((X p Y) => (X q Y))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        REQUIRE(answers_contain(collector, "a q b"));

        interactive.process(".prune-facts (k is on)");

        // The generated rule is still there ...
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p Y) => (X q Y)"));

        // ... and still fires, on a fact entered after the premise was gone.
        interactive.process("c p d");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("derived rules: two generators that write the same rule write it once")
{
    // Two DIFFERENT generators, each with its own premise and its own variable
    // names, whose inner rules are alpha-equivalent. The first one asserts the
    // rule; the second one rebuilds it, lands on the node its own outer rule
    // only MENTIONS, and has to recognise the already-asserted copy instead of
    // renaming its way to a second one. That recognition is the same
    // rules_alpha_equivalent the parser uses -- this is the only path on which
    // the ENGINE depends on it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
k is on
j is ready
a p b
(K is on) => ((X p Y) => (X q Y))
(J is ready) => ((A p B) => (A q B))
)");
        interactive.run(true, false, false);

        // Three rules, not four: the two generators and ONE derived rule.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));
        CHECK(collect_answers(collector).size() == 1); });
}

TEST_CASE("derived rules: a generated rule may be a contradiction rule")
{
    // `!` as the consequence is rebuilt like any other, so a generator can
    // install a contradiction CHECK -- which is the shape that matters for an
    // ontology: the axioms of the check are data, and the check itself is
    // written by a rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
checks is on
bright opp dark
yellow ~ bright
yellow ~ dark
(K is on) => ((X opp Y, A ~ X, A ~ Y, X != Y) => !)
)");
        interactive.run(true, false, false);

        CHECK(has_contradiction(collector));

        // The generated rule is a rule like any other, and the premises of
        // the contradiction are the four facts that satisfied it.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

TEST_CASE("derived rules: the outer variables substitute in every position")
{
    // Not just the predicate slot. Here the generator's variables land in the
    // predicate of the inner rule's condition, in the predicate of its
    // consequence, and inside a COMPOSITE subject and a composite object of
    // that consequence -- all four at once, which is what tells a shallow
    // substitution from a structural one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p lifts q
a p b
(P lifts Q) => ((X P Y) => ((X Q Y) Q (Y P X)))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p Y) => ((X q Y) q (Y p X))"));

        collector.clear();
        interactive.process("S q O");
        // The composed fact, and the two inner facts it is composed of.
        CHECK(answers_contain(collector, "(a q b) q (b p a)"));
        CHECK(answers_contain(collector, "a q b"));
        CHECK(answers_contain(collector, "b q a")); });
}

TEST_CASE("derived rules: a generator fires on a fact another rule derived")
{
    // The generator's own premise is DERIVED, not asserted, so the run has to
    // reach a rule that writes a rule through a rule -- and then let the
    // written rule see facts that are older than itself. Both halves of the
    // fixpoint loop are needed: the one that notices the rule set grew, and
    // the classic pass that shows an old fact to a new rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
q p r
r p s
a marks p
(X marks R) => (R is transitive)
(R is transitive) => ((X R Y, Y R Z) => (X R Z))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "q p s"));

        // Three rules: the two generators and the transitivity rule for p.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3); });
}

TEST_CASE("derived rules: fifty declarations make fifty rules and the run still settles" * doctest::test_suite("slow"))
{
    // One generator against fifty relations, each with a two-step chain to
    // close. What this adds over the small cases is the fixpoint loop's
    // stopping condition at a size where a missing "the set did not grow this
    // time" would show: a second run must add no rule and no fact.
    std::string network = "(R is transitive) => ((X R Y, Y R Z) => (X R Z))\n";
    for (int i = 0; i < 50; ++i)
    {
        const std::string n = std::to_string(i);
        network += "r" + n + " is transitive\n";
        network += "a" + n + " r" + n + " b" + n + "\n";
        network += "b" + n + " r" + n + " c" + n + "\n";
    }

    run_both_modes([&network](auto& collector, auto& interactive)
                   {
        process_lines(interactive, network);
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 51);

        // Every relation closed.
        collector.clear();
        interactive.process("S r0 O");
        CHECK(answers_contain(collector, "a0 r0 c0"));

        collector.clear();
        interactive.process("S r49 O");
        CHECK(answers_contain(collector, "a49 r49 c49"));

        // And it is a fixpoint.
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 51);

        collector.clear();
        interactive.process("S r0 O");
        CHECK(collect_answers(collector).size() == 3); });
}

TEST_CASE("derived rules: a generator inside a cluster is dropped with it")
{
    // A cluster is the experiment scope, and a generator run inside one
    // creates nodes the user never typed: the rule node, its condition set,
    // its consequence patterns and the facts they derive. Rolling the
    // experiment back has to take all of it, or the next experiment starts
    // from a graph nobody described.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        process_lines(interactive, R"(
k is on
a p b
(K is on) => ((X p Y) => (X q Y))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        REQUIRE(answers_contain(collector, "a q b"));

        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("derived rules: a typed rule and a derived one are the same rule, either order")
{
    // `HANDOVER.md` carried this as a known gap -- "a user-typed rule that is
    // alpha-equivalent to a derived one is not recognised as the same rule;
    // both exist, both fire". It is not so, in either direction, and the note
    // was corrected when this case was written.
    //
    // The two directions fail differently if they ever break, which is why
    // both are here: typing first makes the GENERATOR find the existing rule
    // (the mention-collision path, which is the only place the engine itself
    // asks for alpha-equivalence), typing second makes the PARSER find the
    // derived one (the ordinary dedup path).
    SUBCASE("the generator finds the rule the user typed")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(A p B) => (A q B)
k is on
a p b
(K is on) => ((X p Y) => (X q Y))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));
        CHECK(collect_answers(collector).size() == 1);
    }

    SUBCASE("the parser finds the rule the generator wrote")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
k is on
a p b
(K is on) => ((X p Y) => (X q Y))
)");
        interactive.run(true, false, false);

        // Different variable NAMES, including the underscore spelling.
        interactive.process("(_foo p _bar) => (_foo q _bar)");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);
    }
}

// ---------------------------------------------------------------------------
// A construction claims the rule in force that says the same.
//
// A construction that would create a rule initially searches for an
// existing one that says the same -- a typed rule authored before its
// generated twin, the twin a switch writes over a typed rule, two
// generators producing a single rule -- and then returns it. The
// construction operates within a scratch cluster, and a claim drops that
// cluster: a rule nested inside the one it built would otherwise stay
// behind as a rule in force unmentioned by anything, and fire.
// ---------------------------------------------------------------------------

TEST_CASE("derived rules: a generator claims the typed rule it would write, with its collection")
{
    // The rule generated for `t go k` holds a collection of its own text;
    // the typed rule holds a matching one. The construction asserts
    // ownership of the typed rule, thus the firing performs a single write,
    // targeting the typed rule's term. Both rules were present and both
    // fired, each directing their output into a collection specific to
    // themselves.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a bucket")
        {
            process_lines(interactive, R"(
(X t Y) => (Y in @{k})
(G go H) => ((X G Y) => (Y in @{H}))
t go k
a t bug1
)");
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"bug1 in @{k bug1}", "k in @{k bug1}"});
        }
        SUBCASE("a collection no binding changes")
        {
            process_lines(interactive, R"(
(X p k) => (X likes @{c})
(G go H) => ((X p H) => (X G @{c}))
likes go k
b p k
)");
            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{c}"});
        }
        SUBCASE("a collection with a variable")
        {
            process_lines(interactive, R"(
(X p k) => (X likes @{(Z q k)})
(G go H) => ((X p H) => (X G @{(Z q H)}))
likes go k
b p k
)");
            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"b likes @{(Z q k)}"});
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

TEST_CASE("derived rules: a typed rule entered after the switch wrote it is that rule")
{
    // The typed twin of the accumulator the switch wrote is identified
    // through the parser's deduplication, causing a single rule to fire and
    // each member to receive an answer just once. Both fired, and each
    // response was delivered twice.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(K is on) => ((X reported Y) => (Y in @{bug1 bug2}))
k is on
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);

        collector.clear();
        interactive.process("S in O");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"bug1 in @{bug1 bug2 bug3}", "bug2 in @{bug1 bug2 bug3}", "bug3 in @{bug1 bug2 bug3}"}); });
}

TEST_CASE("derived rules: a claim leaves nothing of the construction in force")
{
    // Each generator writes a switch that has already been typed. The
    // construction claims the typed switch, and the nested rule it built
    // during its formation, a rule in force though nothing mentions it, goes
    // with the scratch cluster: nothing is derived, as no switch is on, and
    // a single switch is listed. Without the claim, the generated switch
    // would have remained beside the typed one, forming a second rule saying
    // the same.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const std::string probe : {R"(
(K is on) => ((X p Y) => (X t Y))
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
a p b
)",
                                        R"(
(K is on) => ((X p Y) => (X t @{c}))
(G go H) => ((K is on) => ((X p Y) => (X G @{c})))
t go k
a p b
)",
                                        R"(
(J is armed) => ((X p Y) => (X t master))
(K is on) => ((J is armed) => ((X p Y) => (X t K)))
master is on
a p b
)",
                                        R"(
(K is on) => ((X p Y) => (X t @{k Y}))
(G go H) => ((K is on) => ((X p Y) => (X G @{H Y})))
t go k
a p k
k p m
)"})
        {
            CAPTURE(probe);
            interactive.process(".new");
            process_lines(interactive, probe);

            collector.clear();
            interactive.process("S t O");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }

        // Two generators that write the same switch: one switch beside them.
        interactive.process(".new");
        process_lines(interactive, R"(
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
(G come H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
t come k
a p b
)");
        collector.clear();
        interactive.process("S t O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3); });
}

TEST_CASE("derived rules: a claim found again on every pass adds nothing")
{
    // Classic evaluation fires the generator on each pass; every pass detects
    // the typed switch, the first by claiming it, the later ones by
    // referencing the previously remembered claim, and contributes no
    // additional elements.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto counts = counts_over_passes(collector, interactive, R"(
(K is on) => ((X p Y) => (X t Y))
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
a p b
)");
        CHECK(counts == std::vector<std::size_t>(4, counts.front()));
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

TEST_CASE("derived rules: a claim found again is not built again")
{
    // The claim is remembered under the generator's statement and the recipe
    // key associated with the binding, ensuring that the firings in
    // subsequent passes return the claimed rule without building its parts
    // in a scratch cluster and dropping them once more -- a construction per
    // firing and pass that made a switch over a typed rule incur a cost
    // several times greater than the typed rule itself. Every pass fires the
    // generator: the counters in `.prof` tell a construction from a
    // remembered claim.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto counter = [&](const std::string& key)
        { return construction_counter(collector, interactive, key); };

        interactive.process(".log -1");
        interactive.process(".prof reset");
        counts_over_passes(collector, interactive, R"(
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
a p b
.cluster typed
(J is on) => ((A p B) => (A t B))
.cluster default
)");
        CHECK(counter("built") == 1);
        CHECK(counter("remembered") == 3);

        // A rule that was claimed and has vanished is not returned: after
        // the cluster holding the typed rule is dropped, the next firing
        // builds the rule and keeps it. The typed rule possesses its own
        // variables and comes last, ensuring that the removal takes no node
        // that something outside the cluster was built on.
        interactive.process(".cluster-drop typed");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);
        interactive.process(".run-once");
        CHECK(counter("built") == 2);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

TEST_CASE("derived rules: a claim is not found again once a removal changed the claimed rule's text")
{
    // The `.remove` of a variable's membership leaves the collection
    // standing (a variable was never an element), thus the typed switch
    // stays, and now states `(A t @{c})` rather than `(A t @{c B})`: no
    // longer the output the generator writes. The previously remembered
    // claim still returned it, and the generator's switch was never
    // written -- unless an unrelated cluster drop in between had emptied
    // the record. The removal changes a rule's text just as a write from
    // outside would, and is noted as such.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.semi-naive off
(J is on) => ((A p B) => (A t @{c B}))
(G go H) => ((K is on) => ((X p Y) => (X G @{c Y})))
t go k
)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 2);

        remove_listed(collector, interactive, "B", "B in @{c B}");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3); });
}

TEST_CASE("derived rules: a claim is not found again once a removal took a tag from the claimed rule's text")
{
    // A negation tag and a conjunction tag each constitute independent
    // facts, and the `.remove` of one leaves the rule standing with a
    // different text: absent `(A r B) ~ negation`, the switch says
    // `((A r B), (A p B)) => (A t B)`, and without the conjunction tag, its
    // conditions form a collection. The construction of the switch's
    // consequence remembered the rule it built from the old text and
    // continued to return it, thus the switch never wrote its new
    // consequence, and `a t b` was never derived. The removal of a tag
    // changes a rule's text in the same manner as the removal of a
    // variable's membership (see above), and is noted as one: the
    // construction is rebuilt.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".log -1");

        SUBCASE("a negation")
        {
            process_lines(interactive, R"(
.semi-naive off
(J is on) => ((A p B, ¬(A r B)) => (A t B))
s is on
)");
            collector.clear();
            interactive.process(".node A");
            const std::string condition = listed_id(collector, "A r B");
            REQUIRE_FALSE(condition.empty());

            const std::uint64_t built = construction_counter(collector, interactive, "built");
            remove_listed(collector, interactive, condition, "(A r B) ~ negation");
            process_lines(interactive, "a r b\na p b");
            CHECK(construction_counter(collector, interactive, "built") > built);

            collector.clear();
            interactive.process("S t O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a t b"});
        }
        SUBCASE("a conjunction")
        {
            process_lines(interactive, R"(
.semi-naive off
(J is on) => ((A p B, A r B) => (A t B))
s is on
)");
            const std::uint64_t built = construction_counter(collector, interactive, "built");
            remove_listed(collector, interactive, "conjunction", "{(A r B) (A p B)} ~ conjunction");
            process_lines(interactive, "a r b");
            CHECK(construction_counter(collector, interactive, "built") > built);
        } });
}

TEST_CASE("derived rules: a rule that binds a typed rule's collection leaves the typed rule as written")
{
    // A rule governing rules binds the collection of `(a p b) => (c q @{d})`
    // and writes a rule stating X's membership within it. The rule it writes
    // refers to the collection's data term, so the typed rule says what it
    // said: typed once more it is a twin, and `(a p b) => (c q @{d Z})`
    // represents an additional rule. If X were written into the typed rule's
    // collection, the typed rule would thereafter say
    // `(a p b) => (c q @{d X})`, the rule typed again with its own text
    // would constitute a second rule beside it, and `@{d Z}` would be
    // regarded as a twin.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 3);
        CHECK(any_output_contains(collector, "(a p b) => (c q @{d})"));

        interactive.process("(a p b) => (c q @{d})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3);

        interactive.process("(a p b) => (c q @{d Z})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 4); });
}

TEST_CASE("derived rules: a run that binds the collections of many typed rules builds and fingerprints each rule a few times")
{
    // A rule over rules binds the collections of n typed rules, one
    // construction each. Each construction builds its rule just once: the
    // rule it keeps is remembered under its statement and key, much like a
    // claim is, and the classic pass that comes after an expanded rule set
    // returns it without rebuilding. Each construction fingerprints the rule
    // it builds, and nothing it writes alters another rule's text. The counts
    // correspond to those from `.prof`, which counts while `.log -1` is
    // active.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        constexpr int n = 20;
        interactive.process(".log -1");
        interactive.process(".auto-run"); // a toggle: off
        for (int i = 1; i <= n; ++i)
        {
            const std::string k = std::to_string(i);
            interactive.process("(a" + k + " p b) => (c" + k + " q @{d" + k + "})");
        }
        interactive.process("(G => (S q C)) => ((X r Y) => (X in C))");
        interactive.process(".prof reset");
        interactive.run(true, false, false);

        CHECK(construction_counter(collector, interactive, "built") == n);
        CHECK(construction_counter(collector, interactive, "fingerprinted") <= 3 * n);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2 * n + 1); });
}

TEST_CASE("derived rules: a construction that keeps its rule is not built again")
{
    // A construction that builds its rule, or finds it in force, is
    // remembered under the generator's statement and the recipe key, as a
    // construction claiming a rule is: classic evaluation fires the
    // generator on each pass, and without the record each pass would build
    // the rule's parts in a scratch cluster, find the rule in force and keep
    // nothing new. Once the rule has vanished, the subsequent firing
    // reconstructs it again.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto counter = [&](const std::string& key)
        { return construction_counter(collector, interactive, key); };

        interactive.process(".log -1");
        interactive.process(".prof reset");
        const auto counts = counts_over_passes(collector, interactive, R"(
(G go H) => ((X p Y) => (X G @{c}))
t go k
a p b
)");
        // On the first pass, the rule is written; on the second, its
        // firing's fact is written.
        CHECK(counts[2] == counts[1]);
        CHECK(counts[3] == counts[1]);
        CHECK(counter("built") == 1);
        CHECK(counter("remembered") == 3);

        remove_listed(collector, interactive, "t", "X t @{c}");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);
        interactive.process(".run-once");
        CHECK(counter("built") == 2);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}

// A rule that binds another rule's own collection writes the collection's
// data term (Zelph::bucket_term_id): the term written by the firings of a
// ground rule for that collection. The term holds what is derived into it,
// and nothing prior: the members from the rule's literal, and the member
// its own statement writes, are introduced alongside that rule's firing.
TEST_CASE("derived rules: a data term holds nothing of a rule's literal before the rule fires")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto out = collect_answers(collector);
            std::ranges::sort(out);
            return out;
        };
        SUBCASE("a collection in a value position")
        {
            // Nothing derived `c q @{d}`, hence no entity holds `d` as
            // data: the generated rule names the term without building its
            // members.
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
            interactive.run(true, false, false);
            CHECK(answers("S in O").empty());

            interactive.process("k r k");
            CHECK(answers("S in O") == std::vector<std::string>{"k in @{k}"});

            interactive.process("a p b");
            CHECK(answers("S in O") == std::vector<std::string>{"d in @{d k}", "k in @{d k}"});
            CHECK(answers("c q O") == std::vector<std::string>{"c q @{d k}"});
        }
        SUBCASE("the rule's bucket")
        {
            // The member `c` that the ground rule's statement writes, and
            // the `d` that its literal holds, are neither data before the
            // rule fires.
            process_lines(interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => ((X q Y) => (X in C))
)");
            interactive.run(true, false, false);
            CHECK(answers("S in O").empty());

            interactive.process("k q k");
            CHECK(answers("S in O") == std::vector<std::string>{"k in @{k}"});
            CHECK(answers("c in O").empty());
            CHECK(answers("d in O").empty());

            interactive.process("a p b");
            CHECK(answers("S in O") == std::vector<std::string>{"c in @{d c k}", "d in @{d c k}", "k in @{d c k}"});
        }
        SUBCASE("the rule's bucket, all in one run")
        {
            // Regardless of which occurs initially during the run -- the
            // generated rule's term or the firing of the ground rule --
            // the firing gives the term the members of the bucket.
            interactive.process(".auto-run"); // a toggle: off
            process_lines(interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => ((X q Y) => (X in C))
k q k
a p b
)");
            interactive.run(true, false, false);
            interactive.process(".auto-run"); // on again
            CHECK(answers("S in O") == std::vector<std::string>{"c in @{d c k}", "d in @{d c k}", "k in @{d c k}"});
        } });
}

TEST_CASE("derived rules: a data term holds no membership that a generated rule's text states in it")
{
    // The generated rule declares `X in` the data term located within its
    // consequence, `(X in C) noted ...`. This membership constitutes a
    // pattern within the rule's text, not an actual member: no query yields
    // it as an answer. Nevertheless, the data term printed it as such in
    // every instance it appeared, `k in @{k X}`, in the deduction, among the
    // answers, and within the generated rule itself, because only a
    // membership situated directly beneath a `=>` was considered part of the
    // rule's text. A membership qualifies as rule text regardless of its
    // position, as long as it is nested within a rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto out = collect_answers(collector);
            std::ranges::sort(out);
            return out;
        };
        SUBCASE("a rule's collection")
        {
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => ((X in C) noted yes))
k r k
)");
            interactive.run(true, false, false);
            CHECK(answers("S in O") == std::vector<std::string>{"k in @{k}"});
            CHECK(answers("S noted O") == std::vector<std::string>{"(k in @{k}) noted yes"});
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r Y) => ((X in @{k}) noted yes)"));

            interactive.process("a p b");
            CHECK(answers("S in O") == std::vector<std::string>{"d in @{d k}", "k in @{d k}"});
        }
        SUBCASE("an empty term")
        {
            // Before any derivation into the term, it holds nothing, and the
            // generated rules printed their own X as its member:
            // `(X r Y) => (X in @{X})` named a collection that holds X, and
            // turned into `(X in @{d})` the moment data arrived. A data term
            // prints its data and nothing beyond, also within a statement
            // that writes to it.
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
(G => (S q C)) => ((X s Y) => ((X in C) noted yes))
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r Y) => (X in @{})"));
            CHECK(any_output_contains(collector, "(X s Y) => ((X in @{}) noted yes)"));

            interactive.process("a p b");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r Y) => (X in @{d})"));
            CHECK(any_output_contains(collector, "(X s Y) => ((X in @{d}) noted yes)"));
        }
        SUBCASE("an empty term of a conjunction set")
        {
            // The same applies to the data term within a rule's
            // conditions, which two generated rules write into: each
            // printed the X of both rules as members, `@{X X}`.
            process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (e f g)) => ((X r Y) => (X in C))
(C => (e f g)) => ((X s Y) => ((X in C) noted C))
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r Y) => (X in @{})"));
            CHECK(any_output_contains(collector, "(X s Y) => ((X in @{}) noted {(c q d) (a p b)})"));
        }
        SUBCASE("a ground member")
        {
            // The expression `m in C` constitutes a ground pattern within
            // the generated rule, one that is not derived through any
            // firing: the term holds `d` after the typed rule has fired,
            // and never `m`, which it printed from the start.
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => ((m in C) noted X))
k r k
)");
            interactive.run(true, false, false);
            CHECK(answers("S in O").empty());
            CHECK(answers("S noted O") == std::vector<std::string>{"(m in @{}) noted k"});

            interactive.process("a p b");
            CHECK(answers("S in O") == std::vector<std::string>{"d in @{d}"});
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r Y) => ((m in @{d}) noted X)"));
        }
        SUBCASE("a ground member that the typed rule's firing derives")
        {
            // `d` serves as the member of the typed rule and represents a
            // ground pattern within the generated rule. The firing of the
            // typed rule found the membership that the generated rule had
            // recorded and did not claim it, hence no query answered
            // `d in @{d}` even though the term holds d. Deriving a
            // statement revokes its mark as a pattern, and a membership of
            // a term that a firing builds is derived.
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => ((d in C) noted X))
k r k
)");
            interactive.run(true, false, false);
            CHECK(answers("S in O").empty());
            CHECK(answers("S noted O") == std::vector<std::string>{"(d in @{}) noted k"});

            interactive.process("a p b");
            CHECK(answers("S in O") == std::vector<std::string>{"d in @{d}"});
            CHECK(answers("c q O") == std::vector<std::string>{"c q @{d}"});
            CHECK(answers("S noted O") == std::vector<std::string>{"(d in @{d}) noted k"});
        }
        SUBCASE("a rule's conjunction set")
        {
            process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (e f g)) => ((X r Y) => ((X in C) noted C))
k r k
)");
            interactive.run(true, false, false);
            CHECK(answers("k in O") == std::vector<std::string>{"k in @{k}"});
            CHECK(answers("S noted O") == std::vector<std::string>{"(k in @{k}) noted {(c q d) (a p b)}"});
            for (const std::string& answer : answers("S in O"))
                CHECK(answer.find('X') == std::string::npos);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X r Y) => ((X in @{k}) noted {(c q d) (a p b)})"));
        } });
}

TEST_CASE("derived rules: two typed rules with equal collections give two generated rules over two terms")
{
    // Each typed rule writes `@{d}` of its own, and each possesses a data
    // term distinct from the others, so the rule over rules writes two
    // rules, each naming one term, and both remain separate from the data
    // that the other fires into. Two rules whose collections are equal by
    // their members are not a single rule: if taken as one, the second
    // binding would write no rule of its own, and the data from both would
    // converge into a single collection.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b) => (c q @{d})
(e p f) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 5);
        CHECK(count_outputs_containing(collector, "(X r Y) => (X in @{") == 2);

        process_lines(interactive, "k r k\na p b\ne p f");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("c q O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c q @{d k}", "c q @{d k}"});
        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).size() == 4);
        CHECK_FALSE(any_output_contains(collector, "X}")); });
}

TEST_CASE("derived rules: a rule that binds a typed rule's collection relates to the typed rule's data")
{
    // The rule, once generated, names the data term the typed rule's firing
    // writes for its collection, so a rule joining the two finds them on a
    // single node.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("in a value position")
        {
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X likes C))
a p b
k r k
m r m
(A likes C, B q C) => (A same B)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S same O");
            auto same = collect_answers(collector);
            std::ranges::sort(same);
            CHECK(same == std::vector<std::string>{"k same c", "m same c"});
        }
        SUBCASE("in a condition")
        {
            // `(X in C) => (X flagged yes)` over the bucket associated with
            // the ground rule reads the members of its term: the one the
            // statement writes and the one the literal holds.
            process_lines(interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => ((X in C) => (X flagged yes))
a p b
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S flagged O");
            auto flagged = collect_answers(collector);
            std::ranges::sort(flagged);
            CHECK(flagged == std::vector<std::string>{"c flagged yes", "d flagged yes"});
        } });
}

// What a rule that binds another rule's collection writes is determined where
// it substitutes the variable, based on what the variable is bound to. Three
// shapes emerge from this principle and represent the documented behaviour,
// fixed in place here.
TEST_CASE("derived rules: a rule restated from its bound parts is a rule over the data term")
{
    // `(G => (S q C)) => (G => (S q C))` writes the typed rule back from its
    // components, with C representing the collection's data term, just as in
    // any rule a construction writes: the result is a different rule, over
    // the same term, adjacent to the typed one. It writes into the identical
    // term as the typed rule, so the data is the same.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("restated")
        {
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => (G => (S q C))
a p b
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 3);
            CHECK(any_output_contains(collector, "(a p b) => (c q @{d})"));
            collector.clear();
            interactive.process("c q O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"c q @{d}"});
        }
        SUBCASE("restated under a switch")
        {
            // The inner rule of the switch is a rule over the term, thus
            // the typed rule is mentioned by nothing and continues to fire.
            process_lines(interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((s is on) => (G => (S q C)))
s is on
a p b
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("c q O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"c q @{d}"});
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 3);
        } });
}

TEST_CASE("derived rules: a rule's collection with a variable bound through a marking fact has a data term no firing writes")
{
    // A rule over the engine's marking facts binds the collection of
    // `(X p Y) => (X likes @{d Y})`, whose firings write a term per binding.
    // The generated rule writes into the collection's data term, the term
    // under no binding, which is never written by any of those firings: the
    // two never meet.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{d Y})
"rule pattern" kind meta
((M in C) ~ K, K kind meta) => ((A r B) => (A in C))
a p b
k r k
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a likes {d b}"});
        collector.clear();
        interactive.process("k in O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k in @{k}"});
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p Y) => (X likes @{d Y})")); });
}

TEST_CASE("derived rules: a construction inside a user's cluster is dropped with it")
{
    // A construction's scratch is merged into the cluster that was active at
    // the moment the construction kept its rule, and a claimed construction
    // results in no residual presence in either, thus restoring the node
    // count to its prior state upon dropping the user's cluster.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a construction that claims a typed rule")
        {
            interactive.process("(K is on) => ((X p Y) => (X t Y))");
        }
        SUBCASE("a construction that keeps its rule")
        {
            interactive.process("x q y");
        }
        const std::size_t before = node_count(collector, interactive);
        process_lines(interactive, R"(
.cluster exp
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
a p b
)");
        REQUIRE(node_count(collector, interactive) > before);
        interactive.process(".cluster-drop exp");
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("derived rules: a switch the generator wrote fires once it is on")
{
    // The construction that claims nothing keeps what it built: the generated
    // switch remains in force and writes its rule when `k is on`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
k is on
a p b
)");
        collector.clear();
        interactive.process("S t O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a t b"}); });
}

TEST_CASE("derived rules: a construction that throws leaves its scratch cluster")
{
    // `¬(c q k)` makes the consequence of the rule the generator writes for
    // `t go k` a fact known to be wrong, causing the construction to throw.
    // The scratch cluster it executed within is dropped, and the previously
    // active cluster is restored before the exception leaves the
    // construction. Left active, the scratch took every node that was
    // entered afterwards, and the next construction's drop removed them:
    // `z1 foo bar` and all rules entered after the failure were lost, and
    // the cluster list named the scratch.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.auto-run
¬(c q k)
(G go H) => ((X G Y) => (c q H))
t go k
)");
    interactive.process(R"js(%(try (zelph/run) ([e] (zelph/out "caught"))))js");
    interactive.process(R"js(%(zelph/out (string "clusters=" (length (zelph/clusters)))))js");
    CHECK(any_output_contains(collector, "caught"));
    CHECK(any_output_contains(collector, "clusters=0"));

    process_lines(interactive, R"(
z1 foo bar
(K is on) => ((X p Y) => (X s Y))
(M make N) => ((K is on) => ((X p Y) => (X M Y)))
s make n
)");
    // The pass fires `t go k` once more, and its construction throws
    // once more, after the switch `s make n` writes was claimed.
    collector.clear();
    interactive.process(R"js(%(try (zelph/run-once) ([e] (zelph/out "caught again"))))js");
    interactive.process(R"js(%(zelph/out (string "clusters=" (length (zelph/clusters)))))js");
    CHECK(any_output_contains(collector, "caught again"));
    CHECK(any_output_contains(collector, "clusters=0"));

    collector.clear();
    interactive.process("S foo O");
    CHECK(collect_answers(collector) == std::vector<std::string>{"z1 foo bar"});
    collector.clear();
    interactive.process(".list-rules");
    CHECK(listed_rules(collector) == 3);
}

TEST_CASE("derived rules: a construction does not start in a cluster that has its scratch's name")
{
    // Whenever a construction yields a rule distinct from the one it
    // constructed, it drops the contents recorded by its scratch
    // cluster. If a cluster activated by the user under the scratch's
    // name exists, it would be that scratch, and the drop would take the
    // user's statements with it, so the construction refuses to start
    // and the run stops with the error.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.auto-run
.cluster __construction
z1 foo bar
(K is on) => ((X p Y) => (X t Y))
(G go H) => ((K is on) => ((X p Y) => (X G Y)))
t go k
)");
    CHECK_THROWS_WITH(interactive.process(".run"), doctest::Contains("already active"));

    collector.clear();
    interactive.process("S foo O");
    CHECK(collect_answers(collector) == std::vector<std::string>{"z1 foo bar"});
    collector.clear();
    interactive.process(".list-rules");
    CHECK(listed_rules(collector) == 2);
}

// ---------------------------------------------------------------------------
// The ground parts of a generated rule are patterns, not claims.
//
// When the parser constructs a typed rule, the ground condition or
// consequence is marked as a pattern (see test_rule_patterns.cpp); a rule
// generated without this label causes every element formed via substitution
// to become a claim right after the rule is created.
// "(G go H) => ((H partof I) => (H is green))" with "now go k" answered
// "k is green" even before any entity was part of anything, and the deduction
// did not surface when the premise arrived, because the fact already existed.
// The cases below pin, shape by shape and including nested ones, that a rule
// generated automatically claims exactly what the same rule manually typed
// claims, and that a claim formulated afterwards transforms the part into
// data as usual.
// ---------------------------------------------------------------------------

TEST_CASE("derived rules: a ground consequence of a generated rule is not a claim")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(G go H) => ((H partof I) => (H is green))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is green");
        REQUIRE_FALSE(answers_contain(collector, "k is green"));

        collector.clear();
        interactive.process(".explain (k is green)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        // The premise comes in, and the deduction is output just as any
        // other would be.
        collector.clear();
        interactive.process("k partof test");
        CHECK(any_deduction_of(collector, "k is green"));

        collector.clear();
        interactive.process("A is green");
        CHECK(answers_contain(collector, "k is green")); });
}

TEST_CASE("derived rules: a ground condition of a generated rule is not a claim")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(G go H) => ((H is on) => (H works yes))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is on");
        REQUIRE_FALSE(answers_contain(collector, "k is on"));
        collector.clear();
        interactive.process("k works X");
        REQUIRE_FALSE(answers_contain(collector, "k works yes"));

        collector.clear();
        interactive.process(".explain (k is on)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        collector.clear();
        interactive.process("k is on");
        CHECK(any_deduction_of(collector, "k works yes")); });
}

TEST_CASE("derived rules: a ground condition next to a variable one is not a claim either")
{
    // Not only a rule devoid of variables: whatever the substitution
    // makes ground constitutes a pattern, while anything remaining
    // variable was never a claim.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((H is on, Z p H) => (H works Z))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is on");
        REQUIRE_FALSE(answers_contain(collector, "k is on"));

        interactive.process("k is on");
        interactive.process("z p k");
        collector.clear();
        interactive.process("k works X");
        CHECK(answers_contain(collector, "k works z")); });
}

TEST_CASE("derived rules: a generated contradiction rule waits for its ground condition")
{
    // With the condition claimed by the rule's own construction, the check
    // reported a contradiction within a knowledge base about which nobody
    // had provided information.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((H is broken) => !)");
        collector.clear();
        interactive.process("now go k");
        REQUIRE_FALSE(has_contradiction(collector));

        collector.clear();
        interactive.process("k is broken");
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("derived rules: a ground negated condition of a generated rule is not a claim")
{
    // The pattern that was claimed met the criteria of its own negation's
    // test: the rule that was generated could never be triggered, and the
    // statement "k is off" answered as a fact.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((X p Y, ¬(H is off)) => (X q Y))");
        interactive.process("a p b");
        interactive.process("now go k");

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));

        collector.clear();
        interactive.process("A is off");
        CHECK_FALSE(answers_contain(collector, "k is off")); });
}

TEST_CASE("derived rules: a part claimed before the generator fires stays a claim")
{
    // Only what the rule's construction creates constitutes a pattern --
    // identical to the boundary of a typed rule ("rule patterns: asserting
    // the statement first keeps it data").
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("k is on");
        interactive.process("(G go H) => ((H is on) => (H works yes))");

        collector.clear();
        interactive.process("now go k");
        CHECK(any_deduction_of(collector, "k works yes"));

        collector.clear();
        interactive.process("A is on");
        CHECK(answers_contain(collector, "k is on")); });
}

TEST_CASE("derived rules: nested and container parts of a generated rule are patterns too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a ground term inside a ground condition")
        {
            interactive.process("(G go H) => (((H a b) c d) => (H e f))");
            interactive.process("now go k");

            collector.clear();
            interactive.process("S a B");
            CHECK_FALSE(answers_contain(collector, "k a b"));
            collector.clear();
            interactive.process("S c D");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a set constant in a ground consequence")
        {
            interactive.process("(G go H) => ((X p H) => (H likes {H}))");
            interactive.process("now go k");

            collector.clear();
            interactive.process("S likes O");
            REQUIRE(collect_answers(collector).empty());

            interactive.process("a p k");
            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "k likes {k}"));
        } });
}

// The subsequent trio of shapes features a ground part positioned UNDER a
// fact that keeps a variable. The construction records each part it creates,
// including these, and marks them immediately upon keeping the rule; a
// lookup that had previously designated the ground parts and stopped at the
// initial non-ground element would leave each of these parts a claim the
// moment the rule was written, and `.explain` would classify it as an axiom.
// The typed rule marks each of them.

TEST_CASE("derived rules: a ground term under a variable subject of a generated consequence is a pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((X p H) => (X likes (H is green)))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is green");
        CHECK_FALSE(answers_contain(collector, "k is green"));

        collector.clear();
        interactive.process(".explain (k is green)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted")); });
}

TEST_CASE("derived rules: a ground term under a variable subject of a generated condition is a pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((X likes (H is green)) => (X q H))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is green");
        CHECK_FALSE(answers_contain(collector, "k is green"));

        collector.clear();
        interactive.process(".explain (k is green)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        // The rule itself operates on the statement that contains the
        // pattern.
        interactive.process("a likes (k is green)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q k")); });
}

TEST_CASE("derived rules: the ground innermost consequence of a generated generator is a pattern")
{
    // The "switch over a schema" pattern described in rule-generators.md,
    // with an innermost consequence that the substitution makes ground.
    // Claimed early, it also incurred the loss of the deduction line:
    // `k partof x` produced no output, since `k is green` had already been
    // established.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(G go H) => ((H is on) => ((H partof I) => (H is green)))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("A is green");
        CHECK_FALSE(answers_contain(collector, "k is green"));

        collector.clear();
        interactive.process(".explain (k is green)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        interactive.process("k is on");
        collector.clear();
        interactive.process("A is green");
        CHECK_FALSE(answers_contain(collector, "k is green"));

        collector.clear();
        interactive.process("k partof x");
        CHECK(any_deduction_of(collector, "k is green")); });
}

TEST_CASE("derived rules: a generated consequence whose only variable sits in a container is a pattern")
{
    // The variable resides within the container in `(k likes {H})`. If
    // considered as ground and already present, the fact would remain
    // unmarked; meanwhile, the construction rebuilds the container and builds
    // `k likes {k}` -- a claim established at the moment the rule was
    // written.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((X p H) => (k likes {H}))");
        interactive.process("now go k");

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (k likes {k})");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));

        interactive.process("a p k");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "k likes {k}")); });
}

// The next three include a fact over a collection that the construction
// rebuilds. The construction records the fact at the moment of its creation
// and marks it with the rule's other components; left unmarked, it would
// subsequently be interpreted as a claim. The typed rule marks it, just as
// it marks every part it builds. The first two cover each position: a
// condition, the consequence itself, and a term positioned beneath a
// consequence that keeps a variable. The third is a switch, whose
// rule gets a collection of its own even though the switch does not bind
// anything within it.

TEST_CASE("derived rules: a generated condition over a rebuilt collection is a pattern")
{
    // Claimed, the condition would make the rule fire on itself: `b p k`
    // would derive `b ok k` from `@{(Y q k)} likes k`, a statement no
    // one has stated. The typed
    // `((@{(Y q k)} likes k), (X p k)) => (X ok k)` derives nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => (((@{(Y q H)} likes k), (X p H)) => (X ok H))");
        interactive.process("now go k");

        // The collection took the binding; a construction that keeps H
        // has nothing to claim.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(Y q k)"));

        interactive.process("b p k");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S ok O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("derived rules: a generated consequence over a rebuilt collection is a pattern")
{
    // Claimed, these facts would answer before any data: `S likes O` using
    // `@{(Y q k)} likes @{(Y q k)}`, and `S r O` using `@{(Y q k)} r k`.
    // The typed rules answer neither.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the consequence itself")
        {
            interactive.process("(G go H) => ((X p H) => (@{(Y q H)} likes @{(Y q H)}))");
            interactive.process("now go k");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(Y q k)"));

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a term below a consequence that keeps a variable")
        {
            interactive.process("(G go H) => ((X p H) => (X likes (@{(Y q H)} r k)))");
            interactive.process("now go k");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(Y q k)"));

            collector.clear();
            interactive.process("S r O");
            CHECK(collect_answers(collector).empty());
        } });
}

TEST_CASE("derived rules: a fact over a collection in the rule a switch turns on is a pattern")
{
    // The switch substitutes nothing into the rule, and the construction
    // builds its collection as one of its own, thus the rule it writes
    // possesses an independent collection and a fresh fact over it. Left
    // unmarked, that fact would make `S likes O` answer `@{(Z q Y)} likes k`
    // once the switch is activated. The typed
    // `(X p Y) => (@{(Z q Y)} likes k)` answers nothing until `b p c`, at
    // which point it answers `@{(Z q c)} likes k` alone.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(K is on) => ((X p Y) => (@{(Z q Y)} likes k))");
        interactive.process("sw is on");

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        interactive.process("b p c");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front() == "@{(Z q c)} likes k"); });
}

TEST_CASE("derived rules: a typed rule over a generated rule's ground part does not fire on it")
{
    // Previously, the order decided the outcome: typed first, the part
    // stayed a pattern; generator first, the typed rule activated
    // immediately on the claimed part.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(G go H) => ((H is on) => (H works yes))");
        interactive.process("now go k");
        interactive.process("(k is on) => (z p q)");

        collector.clear();
        interactive.process("z p Q");
        CHECK_FALSE(answers_contain(collector, "z p q")); });
}

TEST_CASE("derived rules: a rule over => does not match a generated rule with variables")
{
    // A pattern that carries a variable constitutes a graph structure, not
    // data, and a generated rule is no exception: a condition over `=>` finds
    // neither the generator nor the rule it wrote. Only rules containing
    // variables are under consideration here; whether a fully ground rule is
    // matched remains undecided (`is_mentioned`) and is deliberately not
    // pinned.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G => H) => (G seen H)
(A likes B) => ((X p B) => (X q B))
tom likes blue
a p blue
)");
        interactive.run(true, false, false);

        // The rule that is generated is present
        // and fires...
        collector.clear();
        interactive.process("S q O");
        REQUIRE(answers_contain(collector, "a q blue"));

        // ...and nothing sees it as data.
        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("derived rules: the ground parts of a generated rule stay patterns across .save and .load")
{
    const auto file = fs::temp_directory_path() / "zelph_generated_pattern_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("(G go H) => ((H is on) => (H works yes))");
        interactive.process("now go k");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("A is on");
        CHECK_FALSE(answers_contain(collector, "k is on"));

        // .load disables auto-run, meaning the claim requires a manual
        // run.
        interactive.process("k is on");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("k works X");
        CHECK(answers_contain(collector, "k works yes"));
    }

    fs::remove(file);
}
