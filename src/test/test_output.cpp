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

#include "test_helpers.hpp"

#include "io/output.hpp"
#include "network/zelph.hpp"

#include <thread>
#include <vector>

using namespace zelph::test;

TEST_CASE(".deductions focus: only input-anchored deductions are printed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Auto-run is active inside run_both_modes: every process() call
        // triggers the reasoning run for that statement itself, with the
        // focus set holding exactly that statement. NO explicit
        // interactive.run calls here -- a second run would execute with an
        // already-cleared focus set and blur what is being tested.
        //
        // Cascade with a CHANGING subject. qq1 derives a qq2 fact about the
        // term (h of A): that subject is materialized during reasoning, but
        // it is materialized OUT OF the entered node, and a statement about
        // (h of socrates) is a statement about socrates -- so focus prints
        // it. qq2 derives qq3 back about A itself, which is the input's own
        // subject.
        //
        // The negative case is a subject that reaches no anchor at all: the
        // atom `sentinel` occurs in the rule and nowhere else. A rule is
        // never an anchor -- with session-wide accumulation, rule anchors
        // would make focus degenerate to "all" for any entered rule set.
        //
        // The rule statements run before collector.clear(), so their echoes
        // cannot contaminate the negative check.
        interactive.process(".deductions focus");
        interactive.process("(A qq1 A) => ((h of A) qq2 (h of A))");
        interactive.process("((h of A) qq2 (h of A)) => (A qq3 A)");
        interactive.process("(A qq1 A) => (sentinel qq4 A)");
        collector.clear();
        interactive.process(":qq1 socrates");
        CHECK(any_deduction_of(collector, "qq3"));
        CHECK(any_deduction_of(collector, "qq2"));
        CHECK_FALSE(any_deduction_of(collector, "qq4"));
        // Every one of them exists regardless of printing (probe).
        interactive.process(R"js(%(string "FOCA-" (zelph/exists "sentinel" "qq4" "socrates")))js");
        CHECK(any_output_contains(collector, "FOCA-true"));
        // Control: in all mode the filtered deduction prints.
        interactive.process(".deductions all");
        collector.clear();
        interactive.process(":qq1 plato");
        CHECK(any_deduction_of(collector, "qq4")); });
}

TEST_CASE(".deductions focus: subterms of the input are not focus anchors")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The top-level fact is asserted via zelph/fact: a bare
        // parenthesized term typed into the REPL is a QUERY, not an
        // assertion. The Janet path takes the same fact() route as the
        // parser, so the capture sees the same bottom-up materialization:
        // the subterm fact (s h1 t) first, then the top-level fact using
        // it as subject -- the numeral-suffix scheme. The focus reduction
        // must keep the top-level fact plus its immediate subject (s h1 t)
        // and object r, but drop the subterm's own components: with the
        // unreduced capture, s anchored (s h2 t) and it printed -- the
        // &42 regression, where every numeral suffix anchored half of the
        // division cascade.
        interactive.process(".deductions focus");
        interactive.process("(A h1 B) => (A h2 B)");
        interactive.process("((A h1 B) g1 C) => ((A h1 B) g2 C)");
        collector.clear();
        interactive.process(R"js(%(zelph/fact (zelph/fact "s" "h1" "t") "g1" "r"))js");
        // Anchored on the input's own subject -> printed.
        CHECK(any_output_contains(collector, "g2"));
        // Anchored on the subterm's component s -> filtered but derived.
        CHECK_FALSE(any_event_contains(collector, "h2"));
        interactive.process(R"js(%(string "FOCB-" (zelph/exists "s" "h2" "t")))js");
        CHECK(any_output_contains(collector, "FOCB-true")); });
}

TEST_CASE(".deductions focus: interactively entered anchors persist across runs")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Focus anchors accumulate over the session: a deduction whose
        // subject (or rule) was entered in an EARLIER statement still
        // prints. Both orders of the classic transitivity demo must show
        // the derived fact -- under per-statement focus, the rule-first
        // order filtered it (subject tim and the rule both stemmed from
        // earlier statements).
        interactive.process(".deductions focus");
        SUBCASE("rule first")
        {
            interactive.process("(R kis ktransitive, A R B, B R C) => (A R C)");
            interactive.process("kancestor kis ktransitive");
            interactive.process("tim kancestor tom");
            collector.clear();
            interactive.process("tom kancestor paul");
            CHECK(any_deduction_of(collector, "tim kancestor paul"));
        }
        SUBCASE("rule last")
        {
            interactive.process("kancestor kis ktransitive");
            interactive.process("tim kancestor tom");
            interactive.process("tom kancestor paul");
            collector.clear();
            interactive.process("(R kis ktransitive, A R B, B R C) => (A R C)");
            CHECK(any_deduction_of(collector, "tim kancestor paul"));
        } });
}

TEST_CASE(".deductions focus: imported statements do not become anchors")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Imports run with input capture suppressed: reasoning-internal
        // facts of the arithmetic stdlib must stay filtered even though
        // every one of its statements passed through process(). The
        // entered query is the only anchor, so exactly its = result
        // prints -- the demo-2.3 acceptance shape, now pinned as a test.
        interactive.process(".deductions focus");
        interactive.process(".import binary-arithmetic");
        collector.clear();
        interactive.process("(&6 * &7) = X");
        CHECK(any_deduction_of(collector, "(&6 * &7) = &42"));
        CHECK_FALSE(any_deduction_of(collector, "mci"));
        CHECK_FALSE(any_deduction_of(collector, "pprod")); });
}

// Concurrent OutputStream flushes must be serialized through the same
// print mutex as emit(): OutputStream copies the handler and flushes
// without a lock of its own, so before the locked_stream fix, parallel
// reasoning workers logging via diagnostic_stream() invoked a stateful
// handler (OutputCollector's vector) concurrently -- heap corruption.
TEST_CASE("output: concurrent stream flushes are serialized")
{
    zelph::io::OutputCollector collector;
    zelph::network::Zelph      z(collector.sink());
    constexpr int              threads = 8;
    constexpr int              lines   = 5000;
    std::vector<std::thread>   workers;
    workers.reserve(threads);
    for (int t = 0; t < threads; ++t)
        workers.emplace_back([&z]
                             {
            for (int i = 0; i < lines; ++i)
                z.diagnostic_stream() << "stress " << i << std::endl; });
    for (auto& w : workers)
        w.join();
    CHECK(collector.events().size() == static_cast<size_t>(threads) * lines);
}

TEST_CASE("commands: an argument a command does not take is an error")
{
    // Most commands already reject surplus arguments; the ones that did not
    // silently did something else instead. ".auto-run" is the trap that
    // matters: it is a toggle sitting among .anchors / .semi-naive /
    // .fact-stores, all of which take [on|off], so ".auto-run off" ENABLED
    // auto-run whenever it happened to be off.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_AS(interactive.process(".auto-run off"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".list-rules everything"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".remove-rules all"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".lang en de"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".deductions loud"), std::runtime_error);

        // The documented forms keep working.
        interactive.process(".auto-run");
        interactive.process(".deductions all");
        interactive.process(".lang en"); });
}

TEST_CASE("naming: renaming a node to the name it already has is a no-op")
{
    // The uniqueness check did not exclude the node itself, so ".name a a"
    // failed with "Name 'a' is already in use by node 11" -- where node 11
    // is 'a'. An idempotent operation should say so, not complain about
    // the very node it was pointed at.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel x");

        collector.clear();
        interactive.process(".name a a");
        CHECK(any_output_contains(collector, "already has this name"));

        // Still there, still reachable under that name.
        collector.clear();
        interactive.process("a rel X");
        CHECK(answers_contain(collector, "a rel x"));

        // A genuine collision is still refused.
        interactive.process("b rel y");
        CHECK_THROWS_WITH_AS(interactive.process(".name a b"),
                             doctest::Contains("already in use"),
                             std::runtime_error); });
}

TEST_CASE("commands: predicate usage counts uses AS a predicate, not facts about it")
{
    // The nodes pointing at a predicate are the facts using it as their
    // relation type PLUS the facts in which it is the SUBJECT -- a fact
    // points at both. Counting them together made every predicate carry its
    // own `pred ~ ->` declaration as a use of itself, so a freshly declared
    // predicate reported one use before anything used it, and each fact
    // ABOUT a predicate inflated the count further. On a Wikidata dump,
    // where every property is an entity with labels and constraints of its
    // own, that is not an off-by-one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a hasPart b");
        interactive.process("hasPart coinedBy alice");
        interactive.process("hasPart coinedBy bob");

        collector.clear();
        interactive.process(".list-predicate-usage");
        CHECK(any_output_contains(collector, "hasPart 1"));
        CHECK(any_output_contains(collector, "coinedBy 2"));
        CHECK_FALSE(any_output_contains(collector, "hasPart 4"));

        // The core vocabulary is declared but unused here, and says so.
        CHECK(any_output_contains(collector, "cons 0"));

        // Same root cause on the value side: the objects of the facts ABOUT
        // hasPart appeared as values OF hasPart, led by the `->` of its own
        // declaration.
        collector.clear();
        interactive.process(".list-predicate-value-usage hasPart");
        CHECK(any_output_contains(collector, "b 1"));
        CHECK(any_output_contains(collector, "Total unique values: 1"));
        CHECK_FALSE(any_output_contains(collector, "alice"));
        CHECK_FALSE(any_output_contains(collector, "bob")); });
}

TEST_CASE("commands: predicate usage agrees with what a query answers")
{
    // The count and the query are two readings of the same question, so a
    // disagreement between them is a bug in one of the two whichever way it
    // points. Pinning them against each other is what keeps the counting
    // path from drifting away from unification's notion of a predicate.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process("c rel d");
        interactive.process("rel coinedBy alice");

        // The count comes FIRST: entering a query materializes its pattern,
        // and that pattern is a fact of `rel` like any other.
        collector.clear();
        interactive.process(".list-predicate-usage");
        const bool counted_two = any_output_contains(collector, "rel 2");

        collector.clear();
        interactive.process("S rel O");
        const size_t answers = collect_answers(collector).size();

        CHECK(answers == 2);
        CHECK(counted_two); });
}

TEST_CASE("commands: a negative count is rejected, not wrapped")
{
    // std::stoull turns "-1" into 18446744073709551615 rather than failing,
    // so ".list -1" announced "Listing 18446744073709551615 nodes" and
    // ".out node -1" quietly meant "all of them". Every count argument goes
    // through one parser now, and that parser looks at the sign.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        interactive.process("a rel b");

        for (const char* cmd : {".list -1", ".clist -1", ".out a -1", ".in b -1",
                                ".list-predicate-usage -1", ".list-predicate-value-usage rel -1"})
        {
            CHECK_THROWS_WITH_AS(interactive.process(cmd),
                                 doctest::Contains("positive number"),
                                 std::runtime_error);
        }

        // Zero was already refused and stays refused; a real count works.
        CHECK_THROWS_AS(interactive.process(".list 0"), std::runtime_error);
        interactive.process(".list 2"); });
}
