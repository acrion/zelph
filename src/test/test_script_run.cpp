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

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// zelph/run and zelph/run-once: reaching the inference engine from Janet.
//
// Auto-run only fires when a REPL input has been processed, so a program that
// drives zelph as a library -- creating facts and rules through the Janet API
// and never entering a statement -- could previously define a rule but never
// obtain its consequences. These tests pin that the two functions run the
// engine, and that they differ in exactly the way the corresponding commands
// do: one pass versus a fixed point.
//
// Auto-run is switched off first, so what is observed is the effect of the
// call and not of the surrounding input processing. Results are reported
// through zelph/out rather than Janet's print, because only the former
// reaches the collector.
// ---------------------------------------------------------------------------

namespace
{
    // Report whether a fact is present, without creating it.
    constexpr const char* kReport =
        R"js(%(defn report [tag s p o] (zelph/out (string tag "=" (if (zelph/exists s p o) "yes" "no")))))js";
} // namespace

TEST_CASE("zelph/run: Janet can trigger forward chaining")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        CHECK(any_output_contains(collector, "Auto-run is now disabled"));
        collector.clear();

        interactive.process(kReport);
        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "mortal")))js");

        // The rule exists, but nothing has run it yet.
        interactive.process(R"js(%(report "before" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "before=no"));

        interactive.process(R"js(%(zelph/run))js");
        interactive.process(R"js(%(report "after" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "after=yes")); });
}

TEST_CASE("zelph/run: reaches a fixed point, run-once does one pass")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();

        interactive.process(kReport);

        // A three-link chain plus transitivity. One pass closes a .. c and
        // b .. d; only a fixed point also yields a .. d.
        interactive.process(R"js(%(zelph/fact "a" "step" "b"))js");
        interactive.process(R"js(%(zelph/fact "b" "step" "c"))js");
        interactive.process(R"js(%(zelph/fact "c" "step" "d"))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "step" 'Y) (zelph/fact 'Y "step" 'Z)] (zelph/fact 'X "step" 'Z)))js");

        interactive.process(R"js(%(zelph/run-once))js");
        interactive.process(R"js(%(report "once-ac" "a" "step" "c"))js");
        interactive.process(R"js(%(report "once-ad" "a" "step" "d"))js");
        CHECK(any_output_contains(collector, "once-ac=yes"));

        interactive.process(R"js(%(zelph/run))js");
        interactive.process(R"js(%(report "full-ad" "a" "step" "d"))js");
        CHECK(any_output_contains(collector, "full-ad=yes")); });
}

TEST_CASE("zelph/run: arity is fixed at zero")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS(interactive.process(R"js(%(zelph/run "unexpected"))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/run-once "unexpected"))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/run-delta "unexpected"))js")); });
}

// ---------------------------------------------------------------------------
// .run-delta / zelph/run-delta
//
// A plain run always opens with a classic pass over the whole graph, so
// "add a little, run again" costs time proportional to everything accumulated
// rather than to the addition. run-delta seeds the fixpoint with the facts
// created since the last run instead.
//
// The tests below pin the two halves of that contract: it must derive what a
// full run would from the new facts, and it must refuse to skip the pass in
// the cases where the pass is exactly what would have found the answer.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kMortalRule =
        R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "mortal")))js";
} // namespace

TEST_CASE("run-delta: derives from the facts added since the last run")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        interactive.process(R"js(%(report "socrates" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "socrates=yes"));

        // New fact, same rules: the delta is all the engine needs.
        interactive.process(R"js(%(zelph/fact "plato" "~" "human"))js");
        interactive.process(R"js(%(report "plato-before" "plato" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "plato-before=no"));

        interactive.process(R"js(%(zelph/run-delta))js");
        interactive.process(R"js(%(report "plato-after" "plato" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "plato-after=yes")); });
}

TEST_CASE("run-delta: falls back to a full pass before any run has happened")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        // Nothing has ever been saturated, so there is no fixpoint to extend.
        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run-delta))js");
        // The notice goes to the diagnostic channel, not to Out.
        CHECK(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "socrates" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });
}

TEST_CASE("run-delta: falls back to a full pass when a rule was added")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        collector.clear();

        // The new rule has to see a fact that is older than it. Seeding from
        // the delta would only offer it the rule's own construction, so the
        // classic pass must not be skipped here.
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "fallible")))js");
        interactive.process(R"js(%(zelph/run-delta))js");
        // The notice goes to the diagnostic channel, not to Out.
        CHECK(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });
}

TEST_CASE("run-delta: seeds facts that arrived through an import")
{
    // Facts created by an imported script are ordinary additions -- the
    // record must not key on input capture, or a program driving zelph as a
    // library (which never enters a statement) would never get a seeded run.
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_run_delta_facts.zph";
    {
        std::ofstream f(script);
        f << "aristotle ~ human\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        collector.clear();

        interactive.process(".import \"" + script.string() + "\"");
        interactive.process(R"js(%(zelph/run-delta))js");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "aristotle" "aristotle" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "aristotle=yes")); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("run-delta: falls back when an import brings a rule")
{
    // A rule arriving with the import has to see the facts that were already
    // there, which is exactly what the skipped classic pass would show it.
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_run_delta_rule.zph";
    {
        std::ofstream f(script);
        f << "(X ~ human) => (X ~ fallible)\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        collector.clear();

        interactive.process(".import \"" + script.string() + "\"");
        interactive.process(R"js(%(zelph/run-delta))js");
        CHECK(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("run-delta: chains, and matches what a full run would derive")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        // Transitivity, so a seeded fact must also combine with older ones
        // rather than only being looked at on its own.
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "step" 'Y) (zelph/fact 'Y "step" 'Z)] (zelph/fact 'X "step" 'Z)))js");
        interactive.process(R"js(%(zelph/fact "a" "step" "b"))js");
        interactive.process(R"js(%(zelph/run))js");

        interactive.process(R"js(%(zelph/fact "b" "step" "c"))js");
        interactive.process(R"js(%(zelph/run-delta))js");
        interactive.process(R"js(%(report "ac" "a" "step" "c"))js");
        CHECK(any_output_contains(collector, "ac=yes"));

        // A second link, added and seeded separately, must still close the
        // three-step chain against what the previous delta produced.
        interactive.process(R"js(%(zelph/fact "c" "step" "d"))js");
        interactive.process(R"js(%(zelph/run-delta))js");
        interactive.process(R"js(%(report "ad" "a" "step" "d"))js");
        CHECK(any_output_contains(collector, "ad=yes")); });
}

// ---------------------------------------------------------------------------
// zelph/rule serves as a macro wrapping zelph/build-rule; zelph/rule*
// functions as the raw procedure. Since a function's arguments are evaluated
// before execution, it cannot know which parts of a rule were constructed
// during the call. The macro runs its argument forms within
// zelph/build-rule, operating inside the scratch cluster and the template
// scope that zelph/dedup-rule provides when a parsed rule is processed,
// ensuring that a rule written with zelph/rule is constructed identically to
// how a typed rule is. Absent the deduplication verification: zelph/rule
// never performs deduplication.
// ---------------------------------------------------------------------------

TEST_CASE("zelph/rule: a rule is not built inside the argument forms of another")
{
    // A rule build executes within a scratch cluster, and a subsequent build
    // nested within it would activate the same scratch, merge it into itself
    // upon completion, and lose what the outer build had recorded. Before the
    // macro, the nested call was not refused: the inner rule was constructed
    // and remained in force independently, while the outer one subsequently
    // failed, as a condition set does not constitute a consequence -- a
    // program that meant a generator got a rule that fires without condition.
    // The rejection occurs before the inner build creates anything, the outer
    // build hands its scratch back as it would in any error case, and the
    // cluster that had been active is active again.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");

        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'K "is" "on")] (zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "q" 'Y))))js"),
                          doctest::Contains("a rule cannot be built while another rule is being built"));
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "q" 'Y)))))js"),
                          doctest::Contains("a rule cannot be built while another rule is being built"));

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "active=" (or (zelph/cluster) "none"))))js");
        CHECK(any_output_contains(collector, "active=exp"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));

        interactive.process("a p b");
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("zelph/rule: zelph/rule* is refused inside the argument forms of another rule")
{
    // Called there, zelph/rule* would build a rule of its own, in force by
    // itself, and return its condition set. The outer call would subsequently
    // fail, as a condition set is not a consequence, and keep what its
    // argument forms had built -- the inner rule among them, which fires
    // without the outer condition: a program that meant a generator, and
    // caught the error, would hold a rule in force from the start. The refusal
    // encountered is the one that zelph/rule meets there, and it occurs before
    // zelph/rule* builds anything, thus neither the inner nor the outer rule
    // leaves a condition set. In a thunk that manually writes a rule for
    // zelph/build-rule, it is likewise refused. The identical parts written as
    // a `=>` fact, as the message suggests, then build the generator within
    // the same session.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(try (zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/rule* [(zelph/fact 'A "q" 'B)] (zelph/fact 'A "r" (zelph/collection "c")))) ([e] (zelph/out (string "caught: " e)))))js");
        CHECK(any_output_contains(collector, "caught: zelph/rule*: a rule cannot be built while another rule is being built"));
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/build-rule (fn [] (zelph/rule* [(zelph/fact 'A "q" 'B)] (zelph/fact 'A "s" 'B)))))js"),
                          doctest::Contains("a rule cannot be built while another rule is being built"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));

        collector.clear();
        interactive.process("S ~ conjunction");
        CHECK(collect_answers(collector).empty());

        interactive.process("c q d");
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());

        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact (zelph/fact 'A "q" 'B) "=>" (zelph/fact 'A "r" (zelph/collection "c")))))js");
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());

        interactive.process("a p b");
        collector.clear();
        interactive.process("S r O");
        CHECK(answers_contain(collector, "c r @{c}")); });
}

TEST_CASE("zelph/rule: a rule nested in another is written as a `=>` fact in its argument forms")
{
    // What the refusal mentioned above tells the program to do instead. The
    // `=>` fact constitutes a part of the outer rule, as the nested rule of
    // a typed generator does: it is not in force on its own, and the outer
    // rule writes it once its condition is satisfied.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'K "is" "on")] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y))))js");
        interactive.process("a p b");
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());

        interactive.process("k is on");
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b")); });
}

TEST_CASE("zelph/rule*: the plain function evaluates its arguments before it runs")
{
    // zelph/rule is a macro, meaning it does not serve as a value that apply
    // or map can receive; zelph/rule* does. Since it is a function, it runs
    // once its arguments have been constructed, thus it cannot tell which of
    // them the call made: a ground part constructed within its arguments
    // constitutes a claim, just as anything zelph/fact builds inside a Janet
    // block does. The purpose of the macro is to mark it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(apply zelph/rule* [(zelph/fact 'X "r" 'Y)] [(zelph/fact 'X "s" 'Y)]))js");
        interactive.process("a r b");
        collector.clear();
        interactive.process("S s O");
        CHECK(answers_contain(collector, "a s b"));

        interactive.process(R"js(%(zelph/rule* [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (zelph/fact "k" "is" "green"))))js");
        collector.clear();
        interactive.process("A is green");
        CHECK(answers_contain(collector, "k is green")); });
}

TEST_CASE("zelph/build-rule: a rule written by hand inside it is built as a typed rule is")
{
    // janet.md's `let` pattern writes the condition set, its tag, and the
    // `=>` fact itself. Executed within zelph/build-rule, what it builds is
    // the rule's text, and the build marks the parts of each `=>` fact over
    // the condition of the one the thunk returns: a ground part answers no
    // query, and the membership of a collection the rule holds is not a data
    // point. Beyond the confines of every rule scope, the identical program
    // builds data and a rule upon it, just as a function's arguments do (the
    // control).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("inside zelph/build-rule")
        {
            interactive.process(R"js(%(zelph/build-rule (fn [] (let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/fact "k" "is" "green")))))))js");
            interactive.process(R"js(%(zelph/build-rule (fn [] (let [condition (zelph/collection (zelph/fact 'X "q" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "has" (zelph/collection "c")))))))js");

            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("a p b");
            CHECK(any_deduction_of(collector, "a likes (k is green)"));
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("inside zelph/build-rule, a rule with two consequences")
        {
            // zelph/rule* writes a single `=>` fact for each consequence
            // over a single condition set, and the macro marks all of
            // them. A thunk that manually constructs this structure
            // returns one `=>` fact, its last form here, while the
            // remaining consequence is equally part of the rule's text as
            // that one.
            interactive.process(R"js(%(zelph/build-rule (fn [] (let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/fact "k" "is" "green"))) (zelph/fact condition "=>" (zelph/fact 'X "loves" (zelph/fact "m" "is" "blue")))))))js");

            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());
            collector.clear();
            interactive.process("A is blue");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("outside every rule scope")
        {
            interactive.process(R"js(%(let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/fact "k" "is" "green")))))js");
            interactive.process(R"js(%(let [condition (zelph/collection (zelph/fact 'X "q" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "has" (zelph/collection "c")))))js");

            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
            collector.clear();
            interactive.process("S in O");
            CHECK(answers_contain(collector, "c in @{c}"));
        } });
}

TEST_CASE("zelph/rule: a collection built before the call is a value the rule writes into")
{
    // What the program built before the call was not built by the rule,
    // hence it does not constitute the rule's own collection: a value that
    // the rule references, much like a typed rule references a named node.
    // Two rules that hold it write to the same node, and a rule that reads
    // it within a condition and writes it within its consequence closes over
    // it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("two rules write into it")
        {
            interactive.process(R"js(%(def C (zelph/collection "c")))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'Y "in" C)))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "q" 'Y)] (zelph/fact 'Y "in" C)))js");
            interactive.process("a p b");
            interactive.process("d q e");

            collector.clear();
            interactive.process("S in O");
            const auto answers = collect_answers(collector);
            CHECK(answers.size() == 3);
            CHECK(any_output_starts_with(collector, "Answer: b in @{"));
            CHECK(any_output_starts_with(collector, "Answer: c in @{"));
            CHECK(any_output_starts_with(collector, "Answer: e in @{"));
        }

        SUBCASE("a rule closes over it")
        {
            interactive.process(R"js(%(def C (zelph/collection "seed")))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "in" C) (zelph/fact 'X "p" 'Y)] (zelph/fact 'Y "in" C)))js");
            interactive.process("seed p a");
            interactive.process("a p b");
            interactive.process("b p c");

            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).size() == 4);
            CHECK(any_output_starts_with(collector, "Answer: c in @{"));
        } });
}

TEST_CASE("zelph/rule: a rule written outside every rule scope keeps its collections as values")
{
    // The `let` pattern executes without zelph/build-rule, involving a
    // collection where one of its members holds a variable. Since that
    // collection was built as data, the firing action writes that specific
    // node, including the variable in full, and each binding shares this
    // instance: `a likes @{(Z q Y)}`, `b likes @{(Z q Y)}`, and a rule that
    // joins on the common node establishes a connection between a and b. This
    // reflects the documented semantics of a rule written outside a rule
    // scope -- the reason janet.md runs the pattern within zelph/build-rule
    // -- not a wanted outcome.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/collection (zelph/fact 'Z "q" 'Y))))))js");
        interactive.process("(X likes C, Y likes C) => (X shares Y)");
        interactive.process("a p k");
        interactive.process("b p m");

        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "a likes @{(Z q Y)}"));
        CHECK(answers_contain(collector, "b likes @{(Z q Y)}"));

        collector.clear();
        interactive.process("S shares O");
        CHECK(answers_contain(collector, "a shares b")); });
}

TEST_CASE("zelph/build-rule: a collection of the rule's text is a term per binding")
{
    // The identical `let` pattern within zelph/build-rule: the collection
    // is the rule's text, thus every firing writes the term of its binding,
    // with the binding substituted, and the two derived facts name two
    // nodes. The rule is the one its text, manually typed, denotes, in
    // either spelling.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/build-rule (fn [] (let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/collection (zelph/fact 'Z "q" 'Y))))))))js");
        interactive.process("((X p Y)) => (X likes @{(Z q Y)})");
        interactive.process("(X p Y) => (X likes @{(Z q Y)})");
        interactive.process("(X likes C, Y likes C, X != Y) => (X shares Y)");
        interactive.process("a p k");
        interactive.process("b p m");

        collector.clear();
        interactive.process("S likes O");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"a likes @{(Z q k)}", "b likes @{(Z q m)}"});

        collector.clear();
        interactive.process("S shares O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(count_outputs_containing(collector, "=>") == 2); });
}

TEST_CASE("zelph/rule: a collection built in the call is the rule's text, one built before it a value")
{
    // The `(zelph/collection "c")` in the argument forms is established
    // within the rule's scope: a collection of the rule's text, with one
    // term for each binding. `C`, created before the call, serves as a
    // value that both bindings write.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "likes" (zelph/collection "c"))))js");
        interactive.process(R"js(%(def C (zelph/collection "d")))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "q" 'Y)] (zelph/fact 'X "loves" C)))js");
        interactive.process("(X likes C, Y likes C, X != Y) => (X shares-liked Y)");
        interactive.process("(X loves C, Y loves C, X != Y) => (X shares-loved Y)");
        process_lines(interactive, "a p b\ne p f\na q b\ne q f");

        collector.clear();
        interactive.process("S shares-liked O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S shares-loved O");
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("zelph/rule: a collection another thread builds while the call runs is a value")
{
    // The rule scope is tied to the thread that writes the rule. A Janet
    // thread that builds `@{q1}` and states `data has @{q1}` while the script
    // thread resides within zelph/rule's argument forms writes data. It got a
    // template id since a rule was being authored somewhere in the process:
    // the generator subsequently bound a collection of rule text and inserted
    // its empty data term into the rule it built, `(X q Y) => (X holds @{})`,
    // so `a holds @{q1}` and `a ok yes` were never obtained through
    // derivation.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A single block: a statement returns only after the threads it
        // started have ended, and the thread waits for the rule's
        // argument forms.
        interactive.process(R"js(%(do (def go (ev/thread-chan 1)) (def done (ev/thread-chan 1)) (ev/thread (fn [] (while (= 0 (ev/count go)) (os/sleep 0.01)) (zelph/fact "data" "has" (zelph/collection "q1")) (ev/give done true)) nil :n) (zelph/rule [(zelph/fact 'X "p" 'Y)] (do (ev/give go true) (while (= 0 (ev/count done)) (os/sleep 0.01)) (zelph/fact 'X "likes" 'Y)))))js");
        process_lines(interactive, "(S has C) => ((X q Y) => (X holds C))\n(X holds C, data has C) => (X ok yes)\na q b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S holds O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a holds @{q1}"});
        collector.clear();
        interactive.process("S ok O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a ok yes"}); });
}

TEST_CASE("zelph/rule: argument forms that wait for the event loop are refused, and nothing of the rule remains")
{
    // The argument forms run within zelph/build-rule, a C call, with the
    // template scope open. A form that waits for the event loop, like
    // `ev/sleep`, pauses execution there, and a C call cannot be resumed: the
    // program went on as though the rule had been written, yet no rule was.
    // Nor could the scope remain open during the wait, because every other
    // fibre in the thread would then write rule text. The call is refused
    // with the cause, the scope is shut down -- a collection built afterwards
    // becomes a value, whose membership is data -- and nothing the forms
    // built remains. The thunk of zelph/rule-text, which writes a rule
    // another statement refers to, operates within the same scope and is
    // likewise refused; it lacks a scratch cluster, so like a failed one it
    // keeps what it constructed before the wait, which in this case is
    // nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        SUBCASE("zelph/rule")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (do (ev/sleep 0) (zelph/fact 'X "likes" (zelph/collection "c")))))js"),
                              doctest::Contains("zelph/rule: the forms that write a rule cannot wait for the event loop"));
        }
        SUBCASE("zelph/rule-text")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule-text (fn [] (ev/sleep 0) (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "likes" (zelph/collection "c"))))))js"),
                              doctest::Contains("zelph/rule-text: the forms that write a rule cannot wait for the event loop"));
        }
        SUBCASE("a refusal the program catches")
        {
            // The wait that was refused registered a wake-up for the fibre
            // executing the chunk, and execution proceeds in that fibre
            // following `try`. With the wake-up left intact, it resumed the
            // fibre at its subsequent wait using the value from the refused
            // wait: the `ev/take` below returned nil before the task had
            // given anything, and without `try`, Janet reported that it
            // cannot resume the terminated fibre. A refusal does not leave
            // a wake-up in its wake.
            interactive.process(R"js(%(do (try (zelph/rule [(zelph/fact 'X "p" 'Y)] (do (ev/sleep 0) (zelph/fact 'X "likes" (zelph/collection "c")))) ([e] nil)) (def ch (ev/chan 1)) (ev/go (fn [] (ev/give ch :late))) (zelph/out (string "taken: " (ev/take ch)))))js");
            CHECK(any_output_contains(collector, "taken: late"));
        }
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));

        interactive.process(R"js(%(zelph/fact "box" "has" (zelph/collection "d")))js");
        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"d in @{d}"}); });
}

TEST_CASE("zelph/fact: a membership in a collection of a rule's text written outside the rule is refused")
{
    // A program keeping the node of a collection it constructed within
    // zelph/build-rule holds a collection of the rule's text. A membership
    // it later writes there, via zelph/fact or as part of a rule that
    // zelph/rule* builds from finished parts, would alter the rule's
    // meaning: the rule would print `(a p b) => (c in @{d c zz})`, and a
    // firing of a rule writing `X in` the collection would put its variable
    // into the data. Once a rule's text is written, it remains fixed, so the
    // write is refused with a message identifying the collection, the rule
    // stays unchanged as originally written, and its firing writes into its
    // data term.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(var t nil))js");
        interactive.process(R"js(%(zelph/build-rule (fn [] (set t (zelph/collection "d")) (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "in" t)))))js");

        // `d` is already a member, so stating it again introduces no new
        // node; however, zelph/fact constitutes a claim, and it turned the
        // rule's literal member into data that `S in O` answered before the
        // rule had fired. It is rejected just like any other write, and the
        // member remains as rule text.
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/fact "d" "in" t))js"), doctest::Contains("is fixed once the rule is written"));
        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        SUBCASE("written by zelph/fact")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/fact "zz" "in" t))js"),
                              doctest::Contains("@{d c} is a collection of a rule's own text, and a rule's text is fixed once the rule is written"));
        }
        SUBCASE("written as the consequence of a rule built from finished parts")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule* [(zelph/fact 'X "r" 'Y)] (zelph/fact 'X "in" t)))js"),
                              doctest::Contains("is fixed once the rule is written"));
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(count_outputs_containing(collector, "=>") == 1);
        CHECK(any_output_contains(collector, "(a p b) => (c in @{d c})"));

        process_lines(interactive, "k r k\na p b");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S in O");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"c in @{d c}", "d in @{d c}"}); });
}
