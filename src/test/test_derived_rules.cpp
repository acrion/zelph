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
}

TEST_CASE("derived rules: a transitivity meta-rule chains the relation it names")
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

        // The meta-rule and exactly one derived rule.
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

TEST_CASE("derived rules: one meta-rule, one derived rule per binding")
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
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        interactive.process("before is transitive");
        interactive.process("a before b");
        interactive.process("b before c");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (a before c)");
        CHECK(any_output_contains(collector, "a before b"));
        CHECK(any_output_contains(collector, "b before c")); });
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
    // The end-to-end case for the feature, and the argument for it: the six
    // property axioms an ontology is usually described with -- transitive,
    // symmetric, sub-property, sub-class, domain, range -- are stated ONCE as
    // rule schemas, and every declaration a modeller writes afterwards is
    // ordinary data that produces its own specialised rule.
    //
    // Nothing here is expressible as a query, and none of it works without
    // rules deriving rules: a schema fires on a DECLARATION and has to leave
    // a rule behind, quantified over the data the declaration says nothing
    // about.
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

        // Six schemas plus one derived rule per declaration.
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

        // The four-step chain, reconstructed: mother -> parent -> person
        // -> agent, each step through a rule that was itself derived.
        collector.clear();
        interactive.process(".explain (m isa agent)");
        CHECK(any_output_contains(collector, "m isa person"));
        CHECK(any_output_contains(collector, "m parent n"));
        CHECK(any_output_contains(collector, "m mother n")); });
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

TEST_CASE("derived rules: a container in a generated consequence follows the renaming")
{
    // A rule generator that substitutes NOTHING into its inner rule -- the
    // switch shape -- has to alpha-rename it, because hash-consing otherwise
    // lands the rebuild on the very node the outer rule only MENTIONS (see
    // "a switch turns a rule on" above). The renaming reached the variables
    // but not the container that holds them, so the derived rule named the
    // container of the rule it was written from:
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

        // The fixpoint arrives: rebuilding the container makes a NEW node,
        // so the generated rule is only recognised as one that already
        // exists if rule identity reads a container by its members. It does,
        // and a further run therefore derives nothing and writes no rule.
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

TEST_CASE("derived rules: a generated rule writing INTO a container keeps that container")
{
    // The counterpart, and the reason the renaming may not simply rebuild
    // every container it passes: `Y in @{X}` says something ABOUT the
    // container, so its identity has to survive. One bucket, named by the
    // generator, for every fact the generated rule derives.
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

        // And it converges: the container is SHARED with the rule the
        // generator mentions, so rule identity has to accept two rules that
        // name the same container node whatever their variables are called.
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
