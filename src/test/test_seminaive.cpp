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

#include "network/reasoning.hpp"

#include <filesystem>

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

namespace
{
    // Before the execution of main(), meaning before the first test
    // constructs anything.
    [[maybe_unused]] const bool check_mode_is_the_default = []
    {
        zelph::network::Reasoning::set_default_seminaive_check(true);
        return true;
    }();
}

TEST_CASE("semi-naive: an engine a test constructs by itself is in check mode too")
{
    // run_both_modes switches check mode on, and for a prolonged duration
    // this was understood to mean the suite operating within it. That
    // understanding was mistaken: 173 out of the 799 test cases at the time
    // built their Interactive directly -- the polynomial-identity pipelines,
    // the cross-check between the two primality modules, the paper scripts
    // -- and the 55 of these that executed inference all ran in the default
    // mode. The default configuration of THIS binary is check mode, which
    // guarantees no test case can leave the net through its engine
    // construction. A case that must not run in this mode declares it
    // explicitly using `.semi-naive on`, and specifies the justification at
    // the declaration site.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive");
    CHECK(any_output_contains(collector, "Semi-naive evaluation: check"));
}

TEST_CASE("semi-naive: classic mode (off) still computes full results (all arithmetic modules)" * doctest::test_suite("slow"))
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

TEST_CASE("semi-naive: a write a negation does not read does not re-match a NAF rule")
{
    // A rule featuring a negated condition took a classic pass across all
    // facts its positive conditions match, at each negation level within a
    // seeded run, and once more following every pass that derived something:
    // with 50 men, an unrelated fact and `.run-delta` re-matched all 50. A
    // seeded run now seeds it at its level from facts that level has
    // not previously encountered, and tests the negation against the bound
    // instance. The counts correspond to those observed under
    // `.semi-naive on`, since check mode's verification pass introduces
    // additional matches of its own.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        interactive.process(".auto-run");
        interactive.process(R"js(%(for i 0 50 (zelph/fact (string "i" i) "~" "man")))js");
        process_lines(interactive, "((X ~ man), ¬(X waits nil)) => (X ~ impatient)\na likes b\n.cluster w\nbob likes cake\n.cluster default\n.run");

        const auto run_delta = [&]
        {
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        };

        SUBCASE("an unrelated fact")
        {
            interactive.process("x likes y");
            run_delta();
            CHECK(any_event_contains(collector, "Reasoning summary: 0 matches processed,"));
        }
        // The drop eliminates a statement that was present when the previous run
        // concluded, thereby adding `likes` to the list of predicates removed
        // since; the negation reads `waits` exclusively.
        SUBCASE("a removal of a fact the negation does not read")
        {
            interactive.process(".cluster-drop w");
            run_delta();
            CHECK(any_event_contains(collector, "Reasoning summary: 0 matches processed,"));
        }
        SUBCASE("a new instance is matched alone")
        {
            interactive.process("i50 ~ man");
            run_delta();
            CHECK(any_event_contains(collector, "Reasoning summary: 1 matches processed,"));
            collector.clear();
            interactive.process("i50 ~ X");
            CHECK(answers_contain(collector, "i50 ~ impatient"));
        } });
}

TEST_CASE("semi-naive: a dropped fact that a negation reads re-matches the NAF rule")
{
    // The scenario for which the classic pass was kept at each level: a
    // statement that was valid during the last run but is now absent can make
    // a negation true for an instance whose positive conditions are all old,
    // and no seed extends to that instance. A removal operation that keeps
    // the record of additions leaves the statement's predicate behind
    // (removed_predicates_since_run), and a NAF rule negating that predicate
    // takes its classic pass. This remained valid as long as every NAF rule
    // took the pass; it pins that the narrower trigger continues to fire.
    // Check mode would likewise detect a miss, constituting a completeness
    // violation in the seeded run.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The `waits` declaration occurs beyond the cluster boundary,
        // ensuring that the single statement is all that the drop
        // takes.
        process_lines(interactive, ".auto-run\njohn ~ man\nx waits y");

        const auto run_delta = [&](const std::string& fact)
        {
            collector.clear();
            interactive.process("john ~ X");
            CHECK_FALSE(answers_contain(collector, fact));
            interactive.process(".cluster-drop w");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            collector.clear();
            interactive.process("john ~ X");
            CHECK(answers_contain(collector, fact));
        };

        SUBCASE("a fact dropped with its cluster")
        {
            process_lines(interactive, "((X ~ man), ¬(X waits nil)) => (X ~ impatient)\n.cluster w\njohn waits nil\n.cluster default\n.run");
            run_delta("john ~ impatient");
        }
        // The first rule establishes `john waits nil` as a pattern, which
        // the cluster claims and its drop marks again: the statement
        // ceases to be data, as though it had been removed.
        SUBCASE("a claim its cluster's drop revokes")
        {
            process_lines(interactive, "(john waits nil) => (x y z)\n((X ~ man), ¬(X waits nil)) => (X ~ impatient)\n.cluster w\njohn waits nil\n.cluster default\n.run");
            run_delta("john ~ impatient");
        }
        // The variable could represent the predicate that has lost a fact.
        // This kind of negation reads what each rule generates, including
        // its own rule's consequence, thus its component lacks a stratified
        // reading as well, and either justification maintains the classic
        // pass.
        SUBCASE("a negation whose predicate a condition binds")
        {
            process_lines(interactive, "((X ~ man), (R ~ watched), ¬(X R nil)) => (X ~ calm)\nwaits ~ watched\n.cluster w\njohn waits nil\n.cluster default\n.run");
            run_delta("john ~ calm");
        }
        // A negated path examines each edge of the predicate it walks, meaning
        // that a dropped edge may cause it to be satisfied by an instance where
        // all other conditions are old.
        SUBCASE("an edge that a negated path walks")
        {
            process_lines(interactive, "((X ~ man), (X P31 D), ¬(D P279∗ banned)) => (X ~ admitted)\njohn P31 c0\nx P279 y\n.cluster w\nc0 P279 banned\n.cluster default\n.run");
            run_delta("john ~ admitted");
        } });
}

TEST_CASE("semi-naive: a NAF rule in a non-stratifiable component keeps its classic pass")
{
    // A component that negates its derived output lacks a stratified reading,
    // and its NAF rules alternate with the positive ones at one level
    // (NegationLevels::negation_inside). A seeded run maintains this
    // alternation unchanged, so an unrelated fact and .run-delta continue to
    // match every man. This held before; it pins that the narrower seeding
    // leaves such a component alone. `.semi-naive on` for the count, as
    // previously stated.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        process_lines(interactive, ".auto-run\n(X ~ man, ¬(X ~ married)) => (X ~ bachelor)\n(X ~ bachelor, X wears ring) => (X ~ married)\ntom ~ man\nbob ~ man\nal ~ man\na likes b\n.run\nx likes y");
        collector.clear();
        interactive.process(".run-delta");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        CHECK(any_event_contains(collector, "Reasoning summary: 3 matches processed,")); });
}

TEST_CASE("semi-naive: a NAF rule a seeded run derives sees the facts older than itself")
{
    // During a run, a rule that appears has seen nothing, and the levels are
    // recalculated with it, meaning that a seeded run ensures each NAF rule
    // receives its classic pass from then on: seeded from what the run
    // drained, the derived rule would never meet `john ~ man`, which predates
    // the run. This remained true as long as each NAF rule took the pass; in
    // check mode, a miss would be reported.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(T ~ trigger) => (((Y ~ man), ¬(Y waits nil)) => (Y ~ impatient))\n((X ~ cat), ¬(X waits nil)) => (X ~ restless)\njohn ~ man\n.run\ngo ~ trigger");
        collector.clear();
        interactive.process(".run-delta");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        collector.clear();
        interactive.process("john ~ X");
        CHECK(answers_contain(collector, "john ~ impatient")); });
}

// At the boundary of its level, a seeded run seeds a NAF rule based on the
// facts that level has not yet seen -- the carried ones and all that the run
// has drained since -- rather than solely on the record of additions. `lonely`
// reads what `impatient` produces at the same level, which that level will
// perceive during its next pass, and `calm` at the next level negates it: both
// depend on facts that no record holds.
TEST_CASE("semi-naive: a seeded NAF pass sees what an earlier pass of the same run derived")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // `impatient` and `lonely` are positioned at the initial level
        // of negation (see .strata), while `calm` resides at the
        // subsequent level.
        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\n((X ~ man), ¬(X waits nil)) => (X ~ impatient)\n((X ~ impatient), ¬(X likes cake)) => (X ~ lonely)\n((X ~ mortal), ¬(X ~ impatient)) => (X ~ calm)\ntim ~ man\n.run");

        const auto run_delta = [&]
        {
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        };

        SUBCASE("on the same negation level")
        {
            interactive.process("john ~ man");
            run_delta();
            collector.clear();
            interactive.process("john ~ X");
            CHECK(answers_contain(collector, "john ~ impatient"));
            CHECK(answers_contain(collector, "john ~ lonely"));
            CHECK_FALSE(answers_contain(collector, "john ~ calm"));
        }
        SUBCASE("on the next negation level")
        {
            process_lines(interactive, "bob waits nil\nbob ~ man");
            run_delta();
            collector.clear();
            interactive.process("bob ~ X");
            CHECK(answers_contain(collector, "bob ~ calm"));
            CHECK_FALSE(answers_contain(collector, "bob ~ impatient"));
        }
        // `.semi-naive on` for the count, since the verification pass in check
        // mode introduces its own matches. Seven: the positive rule matches
        // john and bob, `impatient` matches both men, `lonely` the single fact
        // that `impatient` derives, and `calm` the two mortals -- whereas a
        // classic pass at every level had matched all 53 men.
        SUBCASE("only the new instances are matched")
        {
            interactive.process(".semi-naive on");
            interactive.process(R"js(%(for i 0 50 (zelph/fact (string "i" i) "~" "man")))js");
            interactive.process(".run");
            process_lines(interactive, "john ~ man\nbob waits nil\nbob ~ man");
            run_delta();
            CHECK(any_event_contains(collector, "Reasoning summary: 7 matches processed,"));
        } });
}

// The negation records, kept in check mode and with `.negation-records on`,
// store every fact a NAF rule produced, along with the bindings under which it
// was generated. During a later run, the negation is re-evaluated and any facts
// whose negated premise now holds are reported as `:naf-final` in a quiet run.
// A fact obtained from a seeded pass must be recorded precisely as one from a
// classic pass; otherwise, its defeat goes unreported.
TEST_CASE("semi-naive: the negation check re-tests a fact a seeded NAF pass derived")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto defeat = [&](const bool count)
        {
            process_lines(interactive, "((X ~ man), ¬(X waits nil)) => (X ~ impatient)\ntim ~ man\n.run\njohn ~ man");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            if (count) CHECK(any_event_contains(collector, "Reasoning summary: 1 matches processed,"));
            interactive.process("john waits nil");
            collector.clear();
            interactive.process(R"js(%(let [nf ((zelph/run-delta :quiet) :naf-final)] (zelph/out (string "naf-final=" (length nf) (if (empty? nf) "" (string " fact=" ((first nf) :fact)))))))js");
            CHECK(any_output_contains(collector, "naf-final=1 fact=john ~ impatient"));
        };

        // In check mode, a quiet run yields the finding rather than raising it.
        SUBCASE("in check mode")
        {
            interactive.process(".auto-run");
            defeat(false);
        }
        // `.semi-naive on`, ensuring the count indicates the derivation came
        // from the seed rather than from a classic pass.
        SUBCASE("with .negation-records on")
        {
            process_lines(interactive, ".auto-run\n.semi-naive on\n.negation-records on");
            defeat(true);
        } });
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

// A leaf whose predicate is a composite statement was keyed by its own
// node, which does not appear as a predicate in any fact from a delta: the
// graph holds `x (a p b) y`, never `x (a p Y) y`. The leaf received no seed,
// so a rule over `x (a p Y) Z` saw only what its classic first pass found,
// and check mode reported the fact derived in a later iteration as missed.
// A concrete statement occupying that place, which a fact involving
// additional objects states, meets the same index. The chain of two rules
// makes the fact arrive in a later iteration.
TEST_CASE("semi-naive: a rule over a composite predicate is seeded by what a later iteration derives")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string rule;
        std::string derived;
        std::string query;
        std::string answer;
        SUBCASE("a pattern")
        {
            rule    = "(x (a p Y) Z) => (Y okp Z)";
            derived = "x (a p b) y";
            query   = "S okp O";
            answer  = "b okp y";
        }
        SUBCASE("a concrete statement that a fact with more objects states")
        {
            rule    = "(x (a p c) Z) => (Z okc c)";
            derived = "x (a p b c) y";
            query   = "S okc O";
            answer  = "y okc c";
        }
        SUBCASE("a concrete statement while no fact has more objects")
        {
            // A concrete composite predicate also qualifies as a wildcard
            // leaf, and in the absence of any fact with multiple objects,
            // the seeded condition takes its relation unchanged: the seed
            // whose predicate is precisely that statement must still reach
            // the rule.
            rule    = "(x (a p c) Z) => (Z okc c)";
            derived = "x (a p c) y";
            query   = "S okc O";
            answer  = "y okc c";
        }
        process_lines(interactive, "(u go v) => (w go t)\n(w go t) => (" + derived + ")\n" + rule + "\n");
        CHECK_NOTHROW(interactive.process("u go v"));
        collector.clear();
        interactive.process(query);
        CHECK(collect_answers(collector) == std::vector<std::string>{answer}); });
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

TEST_CASE("semi-naive: a write no path walks does not re-match a path rule")
{
    // A path condition used to make its rule delta-unsafe (refer to the case
    // following these two), and such a rule was executed in a classic manner
    // during each iteration of every run: with 50 instances of a class, an
    // unrelated fact and `.run-delta` re-matched all 50, as did the second
    // iteration of the run that derived their consequences. On a Wikidata
    // graph, every class rule includes such a condition. The rule now takes
    // its classic pass solely in an iteration where the delta extends the
    // predicate it walks. The counts correspond to those of `.semi-naive on`,
    // since check mode's verification pass introduces additional matches of
    // its own.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        interactive.process(".auto-run");
        interactive.process(R"js(%(for i 0 50 (zelph/fact (string "i" i) "P31" "bigclass")))js");
        process_lines(interactive, "bigclass P279 top\ntop P279 root\n(X P31 D, D P279∗ root) => (X is mortal)");

        SUBCASE("the run that derives its consequences matches it once")
        {
            collector.clear();
            interactive.process(".run");
            CHECK(any_event_contains(collector, "Reasoning summary: 50 matches processed,"));
        }
        SUBCASE("an unrelated fact before .run-delta matches it not at all")
        {
            interactive.process(".run");
            interactive.process("x likes y");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            CHECK(any_event_contains(collector, "Reasoning summary: 0 matches processed,"));
        } });
}

TEST_CASE("semi-naive: a new edge of the walked predicate re-matches the path rule")
{
    // The other half of the case above: an edge the walk gains necessitates
    // that such a rule be revisited, even when it arrives between two runs
    // and .run-delta takes it from the record of additions -- no condition of
    // the rule names that predicate, hence no seed would offer it to the
    // rule. This remained valid while the rule took a classic pass in each
    // iteration; it pins that the narrower trigger continues to fire. A miss
    // would likewise be signalled by check mode, manifesting as a
    // completeness violation of the seeded run.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");

        SUBCASE("a path over a named predicate")
        {
            process_lines(interactive, "(A P31 C, C P279⁺ T) => (A below T)\na P31 c1\nc1 P279 c2\n.run\nc2 P279 c3");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            collector.clear();
            interactive.process("A below B");
            CHECK(answers_contain(collector, "a below c2"));
            CHECK(answers_contain(collector, "a below c3"));
        }
        // The path sugar interprets an uppercase letter in the predicate
        // position as a name, meaning a predicate variable comes from
        // Janet. The specific predicate that such a path traverses is
        // determined solely by binding, which keeps its rule in a
        // classic pass at each iteration.
        SUBCASE("a path whose predicate another condition binds")
        {
            interactive.process(".lang zelph");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'R "~" "hierarchy") (zelph/fact 'A "member-of" 'C) (zelph/fact (zelph/fact 'C 'R 'T) "closure" "one-or-more")] (zelph/fact 'A "within" 'T)))js");
            process_lines(interactive, "P279 ~ hierarchy\na member-of c1\nc1 P279 c2\n.run\nc2 P279 c3\n.run-delta");
            collector.clear();
            interactive.process("A within B");
            CHECK(answers_contain(collector, "a within c2"));
            CHECK(answers_contain(collector, "a within c3"));
        } });
}

// A rule featuring both a negated condition and a path condition is seeded at
// its negation level via its remaining conditions, just like any NAF rule, and
// performs a classic pass at that level if the facts the level has not yet
// encountered contain an edge belonging to the predicate traversed by the path:
// no seed could supply such an edge to the rule. In check mode, a missed
// derivation would be reported as a completeness violation of the seeded run.
TEST_CASE("semi-naive: a new edge of the walked predicate re-matches a NAF rule's path")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        constexpr const char* wild = "((X P31 D), (D P279∗ root), ¬(X waits nil)) => (X ~ wild)";

        const auto wild_after_run_delta = [&]
        {
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            collector.clear();
            interactive.process("a ~ X");
            CHECK(answers_contain(collector, "a ~ wild"));
        };

        SUBCASE("an edge entered between two runs")
        {
            process_lines(interactive, std::string(wild) + "\na P31 c1\nc1 P279 c2\n.run\nc2 P279 root");
            wild_after_run_delta();
        }
        // The edge originates in the positive part of the seeded run and
        // reaches the level through what the run has drained, rather than
        // through the record of additions.
        SUBCASE("an edge the seeded run derives")
        {
            process_lines(interactive, std::string("(X sub Y) => (X P279 Y)\n") + wild + "\na P31 c1\nc1 P279 c2\n.run\nc2 sub root");
            wild_after_run_delta();
        }
        // `.semi-naive on` for the count, since the verification pass in
        // check mode introduces additional matches.
        SUBCASE("a write the path does not walk matches it not at all")
        {
            interactive.process(".semi-naive on");
            interactive.process(R"js(%(for i 0 50 (zelph/fact (string "i" i) "P31" "bigclass")))js");
            process_lines(interactive, std::string(wild) + "\nbigclass P279 root\n.run\nx likes y");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            CHECK(any_event_contains(collector, "Reasoning summary: 0 matches processed,"));
        } });
}

// The identical seeded join, featuring a condition where all its variables
// reside within the members of a set pattern. The planner read a condition's
// variables via collect_variables, which does not enter the set in a rule's
// text: it scored the set as a bound atom and the condition as trivially
// connected, and it did not bind any of its variables for subsequent
// conditions. The sole alteration was in order -- unification pairs the
// members in either order, yielding the same result -- thus, the order is
// what the log is asked for. The seeded join arranges only the remaining
// conditions, thereby maintaining its distinction from the classic first
// iteration's line, which orders all conditions.
TEST_CASE("seminaive: a condition whose variables sit in a set pattern is planned by those variables")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("unbound, it is a free condition")
        {
            process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
v0 near w1
(V go V) => (V trig Q)
(V trig Q, V near W, ({(X enters C) (C ~ card)} can nil)) => (W res X)
)");
            interactive.process(".log 3");
            collector.clear();
            interactive.process("v0 go v0");
            interactive.process(".log 0");

            // V is bound by the seed, thus `V near W` is connected; the
            // set's condition shares no variable with the seed.
            CHECK(any_event_contains(collector, "Final order: [0]=near [1]=can"));
            collector.clear();
            interactive.process("S res O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"w1 res john"});
        }
        // A query's set is a value, not rule text, and it is planned the same.
        SUBCASE("unbound, in a query, it is a free condition")
        {
            process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
a near w1
)");
            interactive.process(".log 3");
            collector.clear();
            interactive.process("a near W, ({(X enters C) (C ~ card)} can nil)");
            interactive.process(".log 0");

            CHECK(any_event_contains(collector, "Final order: [0]=near [1]=can"));
            CHECK(answers_contain(collector, "a near w1, {(john enters card1) (card1 ~ card)} can nil"));
        }
        SUBCASE("chosen, it binds them for the conditions after it")
        {
            process_lines(interactive, R"(
{(v0 enters card1)} can nil
card1 ~ red
k big p1
(V go V) => (V trig Q)
(V trig Q, ({(V enters C)} can nil), k big P, C ~ red) => (C res P)
)");
            interactive.process(".log 3");
            collector.clear();
            interactive.process("v0 go v0");
            interactive.process(".log 0");

            // After the set's condition binds C, `C ~ red` becomes connected
            // and comes before the unconnected `k big P`.
            CHECK(any_event_contains(collector, "Final order: [0]=can [1]=~ [2]=big"));
            collector.clear();
            interactive.process("S res O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"card1 res p1"});
        }
        SUBCASE("in subject position, the set is bound only when all its variables are")
        {
            process_lines(interactive, R"(
{(v0 enters card1)} can nil
v0 near w1
(V go V) => (V trig Q)
(V trig Q, ({(V enters C)} can nil), V near W) => (C res W)
)");
            interactive.process(".log 3");
            collector.clear();
            interactive.process("v0 go v0");
            interactive.process(".log 0");

            // Both conditions are connected via V, yet only `V near W`
            // features a subject that the seed binds; the set continues to
            // contain C.
            CHECK(any_event_contains(collector, "Final order: [0]=near [1]=can"));
            collector.clear();
            interactive.process("S res O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"card1 res w1"});
        } });
}

// An engine responsible for loading a rule from a file has not written any
// collection of the rule's text, and the load switches its variable store
// off: the planner's knowledge of the rule's set comes solely from the walk.
// The file is written by a distinct engine, which ceases to exist before the
// initiation of the engine that performs the loading.
TEST_CASE("seminaive: a set pattern in a rule a file brought is planned by its variables too")
{
    const std::string bin = (std::filesystem::temp_directory_path() / "zelph_seminaive_set_plan.bin").string();
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive writer(collector.sink());
        process_lines(writer, R"(
{(john enters card1) (card1 ~ card)} can nil
v0 near w1
(V go V) => (V trig Q)
(V trig Q, V near W, ({(X enters C) (C ~ card)} can nil)) => (W res X)
)");
        writer.process(".save \"" + bin + "\"");
    }

    run_both_modes([&bin](auto& collector, auto& interactive)
                   {
        interactive.process(".load \"" + bin + "\"");
        interactive.process("v0 go v0");
        interactive.process(".log 3");
        collector.clear();
        interactive.process(".run");
        interactive.process(".log 0");

        CHECK(any_event_contains(collector, "Final order: [0]=near [1]=can"));
        collector.clear();
        interactive.process("S res O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"w1 res john"}); });

    std::filesystem::remove(bin);
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
