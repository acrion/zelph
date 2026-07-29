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

#include <set>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// A disjointness audit in miniature -- the Wikidata use case, small enough to
// count by hand.
//
// It is here because it exercises the parts of the engine TOGETHER, which is
// where the interesting failures live: meta-rules quantifying over predicates
// (transitive, symmetric, inverse), a transitive closure feeding a second
// closure (disjointness inherited down the class hierarchy), instance
// inheritance on top of both, an != guard, and finally a contradiction rule
// reading the result of all of it.
//
// The numbers below are what makes it a test rather than a demo: a violation
// is reachable along several derivation paths, and each of them used to be
// reported separately -- differently often per evaluation strategy.
// ---------------------------------------------------------------------------

namespace
{
    // Meta-rules plus a taxonomy in which exactly one thing is wrong.
    const char* const ontology = R"zelph(
(R is transitive, X R Y, Y R Z) => (X R Z)
(R is symmetric, X R Y)         => (Y R X)
(R "is inverse of" S, X R Y)    => (Y S X)

subclassof is transitive
disjointwith is symmetric
subclassof "is inverse of" hassubclass

(X instanceof C, C subclassof D) => (X instanceof D)
(A disjointwith B, C subclassof A) => (C disjointwith B)
(X instanceof A, X instanceof B, A disjointwith B, A != B) => !

dog     subclassof mammal
mammal  subclassof animal
animal  subclassof organism
rock    subclassof mineral

animal disjointwith mineral

rex instanceof dog
)zelph";

    std::size_t contradiction_lines(const zelph::io::OutputCollector& collector)
    {
        std::size_t n = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("! ⇐", 0) == 0) ++n;
        return n;
    }
}

TEST_CASE("ontology: closures compose across meta-rules")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ontology);
        interactive.run(true, false, false);

        SUBCASE("transitive closure of the taxonomy")
        {
            collector.clear();
            interactive.process("dog subclassof X");
            CHECK(answers_contain(collector, "dog subclassof mammal"));
            CHECK(answers_contain(collector, "dog subclassof animal"));
            CHECK(answers_contain(collector, "dog subclassof organism"));
        }
        SUBCASE("the inverse predicate is closed over too")
        {
            collector.clear();
            interactive.process("organism hassubclass X");
            CHECK(answers_contain(collector, "organism hassubclass dog"));
            CHECK(answers_contain(collector, "organism hassubclass mammal"));
        }
        SUBCASE("instances inherit every superclass")
        {
            collector.clear();
            interactive.process("rex instanceof X");
            CHECK(answers_contain(collector, "rex instanceof mammal"));
            CHECK(answers_contain(collector, "rex instanceof organism"));
        }
        SUBCASE("disjointness travels down the hierarchy, in both directions")
        {
            collector.clear();
            interactive.process("dog disjointwith X");
            CHECK(answers_contain(collector, "dog disjointwith mineral"));
            CHECK(answers_contain(collector, "dog disjointwith rock"));
        }
        SUBCASE("a consistent taxonomy produces no contradiction")
        {
            CHECK_FALSE(has_contradiction(collector));
        } });
}

TEST_CASE("ontology: a violation is reported once per instantiation, not once per path")
{
    // rex is a dog and a rock, so it is an instance of {dog, mammal, animal,
    // organism} and of {rock, mineral}. Four of those pairs are declared
    // disjoint in BOTH directions, which is what makes the count worth
    // pinning: every one of them is reachable from several newly derived
    // premises, and semi-naive evaluation seeds a rule once per premise.
    //
    // What must hold is not a particular number but that the number is a
    // property of the DATA: identical for every evaluation strategy, and
    // equal to the number of distinct lines printed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ontology);
        interactive.process("rex instanceof rock");
        collector.clear();
        interactive.run(true, false, false);

        CHECK(has_contradiction(collector));

        std::set<std::string> distinct;
        for (const auto& e : collector.events())
        {
            const std::string t = normalize(e.text);
            if (t.rfind("! ⇐", 0) == 0) distinct.insert(t);
        }

        // No line repeats: one report per instantiation.
        CHECK(distinct.size() == contradiction_lines(collector));
        CHECK(distinct.size() >= 4); });
}

TEST_CASE("ontology: the evaluation strategy does not change the violation count")
{
    // The same graph audited twice. run_both_modes covers parallelism; this
    // covers the fixpoint strategy, which is where the counts diverged --
    // 10 semi-naive against 6 classic for 3 real violations, because a
    // contradiction has no result node that hash-consing could collapse.
    const auto count_for = [](const char* strategy)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(strategy);
        process_lines(interactive, ontology);
        interactive.process("rex instanceof rock");
        collector.clear();
        interactive.run(true, false, false);
        return contradiction_lines(collector);
    };

    const std::size_t seminaive = count_for(".semi-naive on");
    const std::size_t classic   = count_for(".semi-naive off");

    CHECK(seminaive > 0);
    CHECK(seminaive == classic);
}
