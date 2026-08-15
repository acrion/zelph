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
#include "network/reasoning.hpp"
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

        // The QUERY comes first here on purpose: entering it materializes its
        // pattern, which is a fact of `rel` in the graph. The count must not
        // move for that -- see "a pattern is not a use" below.
        collector.clear();
        interactive.process("S rel O");
        const size_t answers = collect_answers(collector).size();

        collector.clear();
        interactive.process(".list-predicate-usage");
        const bool counted_two = any_output_contains(collector, "rel 2");

        CHECK(answers == 2);
        CHECK(counted_two); });
}

TEST_CASE("commands: a pattern is not a use")
{
    // Both listings read every fact that carries the predicate, and a rule's
    // conditions and consequences are such facts -- so was the pattern of the
    // query the user had just typed. None of them was asserted by anybody, and
    // unification refuses to match them, so the listings reported what no
    // query can reproduce: one rule made `p` carry two uses where one fact
    // used it, and the rule's own variable `Y` showed up as a VALUE of `p`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(X p Y) => (X q Y)
(m r n) => (s t u)
)");

        // The query materializes a third pattern, `S p O`.
        collector.clear();
        interactive.process("S p O");
        const size_t answers = collect_answers(collector).size();
        CHECK(answers == 1);

        collector.clear();
        interactive.process(".list-predicate-usage");
        CHECK(any_output_contains(collector, "p 1")); // the fact, not the rule and not the query
        CHECK(any_output_contains(collector, "q 1")); // the DERIVED fact, not the consequence pattern
        CHECK(any_output_contains(collector, "r 0")); // a ground rule pattern, marked and not data
        CHECK(any_output_contains(collector, "t 0"));

        collector.clear();
        interactive.process(".list-predicate-value-usage p");
        CHECK(any_output_contains(collector, "b 1"));
        CHECK(any_output_contains(collector, "Total unique values: 1"));

        collector.clear();
        interactive.process(".list-predicate-value-usage r");
        CHECK(any_output_contains(collector, "Total unique values: 0")); });
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

TEST_CASE("commands: the exploration commands address a fact the way it prints")
{
    // Printed output is meant to read back as input, and .explain and the
    // prune commands took a printed FACT all along. .node, .out and .in did
    // not: ".node a rel b" was refused with "At most one argument required",
    // and ".out (a rel b)" with "Unknown node". The only way to inspect a
    // fact node was to hunt down its numeric ID -- although the fact is
    // exactly what the user had just seen printed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process("x (a rel b) y");

        // Both spellings: bare, and the parenthesised form the renderer uses
        // for a fact in subject or predicate position.
        for (const char* form : {".node a rel b", ".node (a rel b)"})
        {
            collector.clear();
            interactive.process(form);
            CHECK(any_output_contains(collector, "Representation: a rel b"));
        }

        collector.clear();
        interactive.process(".out (a rel b)");
        CHECK(any_output_contains(collector, "Outgoing connected nodes"));

        collector.clear();
        interactive.process(".in (a rel b)");
        CHECK(any_output_contains(collector, "Incoming connected nodes"));

        // The trailing count still separates, and a name or an ID still
        // resolves the way it did.
        collector.clear();
        interactive.process(".out (a rel b) 1");
        CHECK(any_output_contains(collector, "first 1 of"));

        collector.clear();
        interactive.process(".out a");
        CHECK(any_output_contains(collector, "Outgoing connected nodes"));

        collector.clear();
        interactive.process(".node 1");
        CHECK(any_output_contains(collector, "Node ID: 1"));

        // A pattern that denotes nothing is an error, not a silent empty
        // listing.
        CHECK_THROWS_AS(interactive.process(".node q nosuchrel r"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".out (q nosuchrel r)"), std::runtime_error); });
}

TEST_CASE("commands: a fact whose object is a numeral is not split by the count")
{
    // The trailing-number ambiguity .explain already had: ".out a rel 5" is
    // the fact, not the node `a rel` with a count of 5. The documented count
    // keeps precedence, so the numeral is only read as part of the pattern
    // when the shorter reading resolves to nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel 5");

        collector.clear();
        interactive.process(".node a rel 5");
        CHECK(any_output_contains(collector, "Representation: a rel 5"));

        collector.clear();
        interactive.process(".out a rel 5");
        CHECK(any_output_contains(collector, "Outgoing connected nodes"));

        // With a count after it, the fact is still the fact.
        collector.clear();
        interactive.process(".out a rel 5 1");
        CHECK(any_output_contains(collector, "first 1 of")); });
}

TEST_CASE("commands: a multi-object fact is addressed whole, numeral object and all")
{
    // The worst case for splitting a trailing count off: a fact with several
    // objects, the last of them a numeral. It resolves whole because the
    // fact one object SHORT does not exist -- entering "a rel b 5" builds one
    // node with two objects, not two facts -- so the count reading has no
    // head to attach to and never wins.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b 5");

        collector.clear();
        interactive.process(".node a rel b 5");
        CHECK(any_output_contains(collector, "Representation: a rel b 5"));

        collector.clear();
        interactive.process(".out a rel b 5");
        CHECK(any_output_contains(collector, "Outgoing connected nodes"));

        // The shorter fact is genuinely absent, and saying so is the point:
        // it is what leaves the count reading no foothold.
        CHECK_THROWS_AS(interactive.process(".node a rel b"), std::runtime_error); });
}

TEST_CASE("commands: a composite predicate is named and counted like any other")
{
    // A fact in predicate position is a first-class predicate -- `logic.md`
    // makes a point of it -- and the two usage listings could neither name
    // nor count it.
    //
    // Naming: get_name is empty for a node the parser never named, and the
    // column was simply blank, so the listing gave a count without saying
    // what it counted.
    //
    // Counting: a COMPOSITE relation is pointed at by its own subject and
    // objects as well, and both passed the role test that separates a
    // predicate from a subject -- the subject through the
    // single-outgoing-edge exemption that `~ ~ ->` needs, the object because
    // a fact does not point back at it. So one use reported three.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x (a p b) y
z (a p b) w
)");
        collector.clear();
        interactive.process(".list-predicate-usage");
        CHECK(any_output_contains(collector, "a p b 2"));
        CHECK_FALSE(any_output_contains(collector, "a p b 4"));

        // The atomic predicate next to it is unaffected: `a p b` is its one
        // use, and the facts that USE that fact are not uses of `p`.
        CHECK(any_output_contains(collector, "p 1")); });
}

TEST_CASE("commands: a fact used as a predicate is not a value of itself")
{
    // The value listing read a fact's objects from its incoming set, keeping
    // whatever the fact does not point back at. Every fact that uses it as a
    // PREDICATE looks exactly like that -- it points at the fact and is not
    // pointed back at -- so `x (a p b) y` made itself a value of `a p b`, and
    // the listing for `p` reported a second, nameless value nobody wrote.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x (a p b) y
)");
        collector.clear();
        interactive.process(".list-predicate-value-usage p");
        CHECK(any_output_contains(collector, "b 1"));
        CHECK(any_output_contains(collector, "Total unique values: 1")); });
}

TEST_CASE("commands: the value listing takes a predicate the way it prints")
{
    // `.list-predicate-value-usage (a p b)` was three arguments and was
    // refused on arity, although a fact in predicate position is exactly what
    // one asks this listing about. Same resolution as .node, .out and .in,
    // trailing count included.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x (a p b) y
z (a p b) w
)");
        for (const char* form : {".list-predicate-value-usage (a p b)",
                                 ".list-predicate-value-usage a p b"})
        {
            collector.clear();
            interactive.process(form);
            CHECK(any_output_contains(collector, "Value Usage for predicate a p b"));
            CHECK(any_output_contains(collector, "y 1"));
        }

        // The trailing count still separates, and a plain name still resolves.
        process_lines(interactive, R"(
a q c
a q d
a q e
)");
        collector.clear();
        interactive.process(".list-predicate-value-usage q 2");
        CHECK(any_output_contains(collector, "Showing top 2 of 3 values"));

        CHECK_THROWS_AS(interactive.process(".list-predicate-value-usage nosuch"), std::runtime_error); });
}

TEST_CASE("commands: the value listing names a value that has no name")
{
    // The sibling listing and this one's own HEADING learned to render a
    // nameless node (02d1597); the value COLUMN was missed, so every value
    // that is not an atom -- a nested fact, a list, a set, a collection --
    // printed as a bare count with nothing in front of it. A listing that
    // cannot name what it counts is not usable output.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x rel (a p b)
z rel <1 2>
w rel named
v rel @{q r}
u rel {s t}
)");
        collector.clear();
        interactive.process(".list-predicate-value-usage rel");
        CHECK(any_output_contains(collector, "Total unique values: 5"));
        CHECK(any_output_contains(collector, "named 1"));
        CHECK(any_output_contains(collector, "a p b 1"));
        CHECK(any_output_contains(collector, "<1 2> 1"));
        CHECK(any_output_contains(collector, "@{q r} 1"));
        CHECK(any_output_contains(collector, "{s t} 1")); });
}

TEST_CASE("commands: the parallel-unification summary counts what it claims")
{
    // "Parallel unifications activated for N distinct fixed relations" read a
    // static set that nothing ever filled: 457b14b introduced TWO statics of
    // that name, one in reasoning.cpp and one in unification.cpp, and only the
    // second was inserted into. The line has therefore said 0 since December
    // 2025, whatever the run did -- a diagnostic that reports a measurement it
    // never takes, which is worse than no diagnostic at all.
    //
    // Not run_both_modes: the number is a statement ABOUT the parallel path,
    // so the mode is the subject of the test rather than something it should
    // be invariant under. All three cases below are the default (parallel)
    // engine except the last, which turns it off.
    const std::string unbound_rule = R"(
a p b
c p d
(X p Y) => (X q Y)
)";

    SUBCASE("both sides unbound: the snapshot path is taken and counted")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, unbound_rule);

        collector.clear();
        interactive.run(true, false, false);
        CHECK(any_event_contains(collector, "Parallel unifications activated for 1"));
    }

    SUBCASE("a bound side keeps the snapshot path out of it")
    {
        // The shape `janet.md` shows reporting 0, which is why that page did
        // not have to be corrected when the counter started measuring.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
socrates ~ human
(X ~ human) => (X ~ mortal)
)");
        collector.clear();
        interactive.run(true, false, false);
        CHECK(any_event_contains(collector, "Parallel unifications activated for 0"));
    }

    SUBCASE("single-core evaluation reports none, by construction")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".parallel");
        process_lines(interactive, unbound_rule);

        collector.clear();
        interactive.run(true, false, false);
        CHECK(any_event_contains(collector, "Parallel unifications activated for 0"));
    }
}

TEST_CASE("listing: a value listing does not fill a cache it never reads")
{
    // A guard on MEMORY, not on a result, and the reason it exists was
    // measured on a live run: .list-predicate-value-usage P31 on the full
    // Wikidata dump grew its own footprint by 6 MiB/s -- ~21 GiB per hour --
    // with fact structures it stores once and never asks for again, on a run
    // that was already swapping. Nothing about the OUTPUT changes when that
    // comes back, which is why the listing tests above cannot see it.
    //
    // Same shape as prune_nodes, which suspends the cache for the same reason.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process("a rel x");
        interactive.process("b rel y");
        interactive.process("c rel x");

        const auto rel = graph->get_node("rel");
        REQUIRE(rel != 0);

        const auto facts = graph->get_facts_of_predicate(rel);
        REQUIRE(facts.size() == 3);

        // Whatever asserting them warmed is not what is under test.
        graph->invalidate_fact_structures_cache();

        interactive.process(".list-predicate-value-usage rel");

        // The listing reconstructed every one of these -- that is how it
        // counted the values -- and kept none of them.
        size_t cached = 0;
        for (const auto fact : facts)
        {
            zelph::network::FactStructurePtr out;
            if (graph->try_get_fact_structures_cached(fact, out)) ++cached;
        }
        CHECK(cached == 0); });
}
