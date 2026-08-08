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

#include <doctest/doctest.h> // provides main()

#include "test_helpers.hpp"

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Semi-naive evaluation
//
// run_both_modes already puts every test into `.semi-naive check` mode, so
// the entire suite doubles as an equivalence test between delta-driven and
// classic evaluation. The tests in this file pin the specific design
// decisions and semantic corner cases of the semi-naive implementation.
//
// NOTE on rule definition order: rules are iterated in definition order
// (adjacency containers preserve insertion order), so defining the CONSUMING
// rule before the PRODUCING rule guarantees that the classic first pass
// cannot complete the derivation chain -- the completion must then happen
// via the delta path (or the delta-unsafe classic re-application), which is
// exactly what these tests exercise. Should iteration order ever change,
// the tests degrade gracefully: the checked property still holds, only the
// cross-iteration path is no longer guaranteed to be the one taken -- and
// check mode independently catches any completeness regression.
// ---------------------------------------------------------------------------

TEST_CASE("semi-naive: .semi-naive command reports and switches modes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // run_both_modes enables check mode for every test.
        collector.clear();
        interactive.process(".semi-naive");
        CHECK(any_output_contains(collector, "Semi-naive evaluation: check"));

        collector.clear();
        interactive.process(".semi-naive off");
        CHECK(any_output_contains(collector, "Semi-naive evaluation: off"));

        collector.clear();
        interactive.process(".semi-naive on");
        CHECK(any_output_contains(collector, "Semi-naive evaluation: on"));

        CHECK_THROWS_AS(interactive.process(".semi-naive banana"), std::runtime_error); });
}

TEST_CASE("semi-naive: classic mode (off) still computes full results (all arithmetic modules)")
{
    // Pins that .semi-naive off remains a fully functional evaluation
    // strategy. The switch happens BEFORE the import on purpose (hence no
    // run_arithmetic_modules): for binary-nand-arithmetic this puts the
    // stratified NAF gate bootstrap itself on the pure classic path.
    auto classic_mode_with = [](const char* module)
    {
        run_both_modes([&](auto& collector, auto& interactive)
                       {
            interactive.process(".semi-naive off");
            interactive.process(std::string(".import ") + module);
            collector.clear();
            interactive.process("&12 * &34");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&12 * &34) = &408)")); });
    };

    SUBCASE("decimal-arithmetic") { classic_mode_with("decimal-arithmetic"); }
    SUBCASE("binary-arithmetic") { classic_mode_with("binary-arithmetic"); }
    SUBCASE("binary-nand-arithmetic") { classic_mode_with("binary-nand-arithmetic"); }
}

TEST_CASE("semi-naive: negation over a growing domain stays complete across iterations")
{
    // The consumer rule ranges over the DOMAIN of the flagged relation, and
    // the producer EXTENDS that domain during reasoning: (t1 flagged good)
    // exists only because the producer derived it. The consumer is defined
    // FIRST (see NOTE above), so in the classic first pass it runs before
    // that fact exists and the derivation is forced across iterations.
    //
    // The domain used to be implicit: `¬(X flagged bad)` alone, with X bound
    // by nothing positive, made the negation enumerate the subjects of
    // `flagged` and bind them. That second reading of `¬` is gone -- it
    // depended on whether some other condition happened to bind the subject
    // -- so the domain is now written down, which is what it always meant.
    // The rule stays the delta hazard it was: `X flagged S` is a positive
    // leaf that gains facts during the run, so a strategy that only seeds
    // from the previous iteration's delta has to pick it up.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M marker K, X flagged S, ¬(X flagged bad)) => (X unflagged K)
(A trigger B) => (B flagged good)
m marker k
a flagged good
)");
        CHECK(any_output_starts_with(collector, "( a unflagged k )"));

        collector.clear();
        interactive.process("s trigger t1");
        interactive.run(true, false, false);
        // t1 entered the flagged domain only through the producer's
        // deduction -- the negation rule must still see it.
        CHECK(any_output_starts_with(collector, "( t1 unflagged k )")); });
}

TEST_CASE("semi-naive: facts materialized as instantiation side effects seed rules")
{
    // Pins the delta-capture design decision (fact()-level observer, not a
    // deduce()-level hook): the producer's consequence ((A bar B) baz ok)
    // materializes the INNER fact (p bar q) as a side effect of
    // instantiate_fact -- it is not itself a deduction. The consumer
    // matches exactly that inner fact at top level. A deduce()-level delta
    // would only contain the baz fact, the consumer would never be seeded,
    // and (p linked q) would be missing (check mode would then fail the
    // run). The consumer is defined first (see NOTE above), so the classic
    // first pass cannot derive it either -- only the seeded path can.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X bar Y) => (X linked Y)
(A foo B) => ((A bar B) baz ok)
)");
        collector.clear();
        interactive.process("p foo q");
        interactive.run(true, false, false);
        CHECK(any_output_starts_with(collector, "( p linked q )")); });
}

TEST_CASE("fact structures: same-predicate parent must not masquerade as subject")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // inner = (x op y), mid = (inner op z), outer = (mid op q). outer
        // uses mid as its SUBJECT under the SAME predicate, so mid <-> outer
        // is bidirectional and the child-fact filter deliberately skips it:
        // pre-fix, get_fact_structures(mid) offered the spurious reading
        // {outer, op, {z}} alongside the genuine {inner, op, {z}}.
        //
        // Why the pattern needs depth 3: unify_nodes commits to the FIRST
        // successful structure pair, so a shallow pattern matches the
        // genuine reading and never reaches the spurious one. The depth-3
        // subject (((F op G) op H) ...) FAILS on the genuine reading of mid
        // (inner's subject x is an atom where the pattern demands structure)
        // and SUCCEEDS on the spurious one (outer provides the extra level):
        // F=inner, I=z -- materializing the junk fact (inner got z),
        // order-independently. This is the distilled jacobian mechanism,
        // where the genuine branch failed under pre-bound variables and the
        // engine fell through to the parent misreading.
        interactive.process(R"js(%(def inner (zelph/fact "x" "op" "y")))js");
        interactive.process(R"js(%(def mid (zelph/fact inner "op" "z")))js");
        interactive.process(R"js(%(def outer (zelph/fact mid "op" "q")))js");
        interactive.process(R"js(%(zelph/fact mid "mark" mid))js");
        interactive.process(R"js(%(zelph/fact outer "mark" outer))js");
        interactive.process("((((F op G) op H) op I) mark (((F op G) op H) op I)) => (F got I)");
        interactive.run(true, false, false);
        collector.clear();
        // Genuine: the mark fact on outer decomposes ((x op y) op z) op q
        // through three same-predicate levels -> (x got q). Pins the
        // false-positive risk of hash verification: genuine same-predicate
        // hash SUBJECTS (mid inside outer, inner inside mid) must keep
        // matching.
        interactive.process(R"js(%(string "FS-GENUINE-" (zelph/exists "x" "got" "q")))js");
        // Junk: only derivable through the spurious reading of mid.
        interactive.process(R"js(%(let [i (zelph/fact "x" "op" "y")] (string "FS-JUNK-" (zelph/exists i "got" "z"))))js");
        CHECK(any_output_contains(collector, "FS-GENUINE-true"));
        CHECK(any_output_contains(collector, "FS-JUNK-false")); });
}

TEST_CASE("seminaive: seeded join order prefers connected conditions over unconnected scans")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // 64 mark self-facts on structured subjects (u_i pair v_i): the
        // variable V shared with the seed sits INSIDE the pattern, invisible
        // to the subject/object boundness scores -- the exact shape of the
        // SC congruence conditions. 16 big facts form the unconnected
        // relation; the sizes give the old scorer a robust 4-point margin
        // in favor of scanning big first (new-vars +4 vs +2, log2 -4 vs -6),
        // so this case is red without the connectivity term.
        interactive.process(R"js(%(each i (range 64) (def p (zelph/fact (string "u" i) "pair" (string "v" i))) (zelph/fact p "mark" p)))js");
        interactive.process(R"js(%(each i (range 16) (zelph/fact (string "x" i) "big" (string "y" i))))js");
        process_lines(interactive, R"(
(V go V) => (V trig qq)
(V trig Q,
 (U pair V) mark (U pair V),
 U big P)
=> (Q res P)
)");
        interactive.process(".log 3");
        collector.clear();
        interactive.process("v0 go v0");

        // Auto-run derives (v0 trig qq); the delta seeds the second rule's
        // trig leaf, binding V and Q. Of the two remaining conditions, only
        // the mark pattern is connected (via V, at structural depth) and
        // must be ordered before the unconnected big scan. The classic
        // first iteration also logs a Final order, but with nothing bound
        // it starts at the trig leaf, so it cannot produce "[0]=big" either.
        CHECK(any_event_contains(collector, "[0]=mark"));
        CHECK_FALSE(any_event_contains(collector, "[0]=big")); });
}

TEST_CASE("semi-naive: a path condition sees a closure that grew during the run")
{
    // A transitive path condition is no fact lookup, and it is worse off than
    // the neural one: the facts it depends on are every edge of the predicate
    // it WALKS, and no condition of the rule names that predicate. Its own
    // predicate is `closure`, so seeding waited for a new tag fact -- which
    // never comes, tag facts being rule structure. The rule fired once and was
    // never revisited when the closure grew underneath it.
    //
    // Semi-naive is the DEFAULT, so this was a wrong answer by default:
    // classic derived `(a below c2)`, semi-naive did not, and `.explain` said
    // "Fact is not asserted". No existing test could see it -- the whole suite
    // runs in check mode, but every other path-condition test enters its chain
    // as DATA, so nothing ever grew the closure mid-run.
    //
    // The consuming rule is defined first on purpose (see the note at the top
    // of this file): the classic first pass then cannot complete the chain, so
    // completion has to come from the delta path or the delta-unsafe
    // re-application -- which is the property under test.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A P31 C, C P279⁺ T) => (A below T)");
        interactive.process("(X sub Y) => (X P279 Y)");
        interactive.process("a P31 c1");
        interactive.process("c1 sub c2");

        collector.clear();
        interactive.process("A below B");
        CHECK(answers_contain(collector, "a below c2"));

        // And it keeps up over more than one step of growth.
        interactive.process("c2 sub c3");
        collector.clear();
        interactive.process("A below B");
        CHECK(answers_contain(collector, "a below c2"));
        CHECK(answers_contain(collector, "a below c3")); });
}
