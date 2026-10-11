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
// zelph/rule serves as a macro that encapsulates a rule's build; zelph/rule*
// functions as the raw procedure. Since a function's arguments are evaluated
// before execution, it cannot know which parts of a rule were constructed
// during the call. The macro runs its argument forms within the build context
// that zelph/build-rule also invokes, operating inside the scratch cluster and
// the template scope that zelph/dedup-rule provides when a parsed rule is
// processed, ensuring that a rule written with zelph/rule is constructed
// identically to how a typed rule is. Absent the deduplication verification:
// zelph/rule never performs deduplication.
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
    // build drops its scratch as it does in any error case, and the
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

TEST_CASE("zelph/rule: the argument forms cannot switch clusters, import, load, save or run")
{
    // The rule's build hosts the argument forms within its scratch cluster.
    // `zelph/cluster` turned the rule's ground part `k is green` into data,
    // which `A is green` responded to, and the build quietly reversed the
    // switch upon reinstating the cluster it had found. The remaining calls
    // change the graph beneath a rule that is half written, and a save wrote
    // the incomplete rule in the file, using its ground condition as data. Each
    // is refused under its own name, the build retains no trace of what the
    // forms built, and the cluster that was active stays active.
    const auto dir      = std::filesystem::temp_directory_path();
    const auto imported = dir / "zelph_rule_args_import.zph";
    const auto loaded   = dir / "zelph_rule_args_load.bin";
    const auto saved    = dir / "zelph_rule_args_save.bin";
    {
        std::ofstream(imported) << "m n o\n";
    }
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        std::filesystem::remove(saved);
        interactive.process(".cluster other");
        interactive.process("x y z");
        interactive.process(".save \"" + loaded.generic_string() + "\"");
        interactive.process(".cluster exp");
        const std::size_t before = node_count(collector, interactive);

        // The message does not indicate which forms are responsible for
        // writing the rule: since the flag operates across the entire
        // process, a call originating from a different thread also encounters
        // the refusal while a rule is being built.
        const auto refused = [&](const std::string& call, const std::string& name)
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (do )js" + call + R"js( (zelph/fact 'X "likes" (zelph/fact "k" "is" "green")))))js"),
                              doctest::Contains(name + ": refused while a rule is being built. Switch or drop a cluster, import a file, load or save a network, "
                                                       "run the engine or query the graph before the rule is built or after it."));
        };
        SUBCASE("zelph/cluster")
        {
            refused(R"js((zelph/cluster "exp2"))js", "zelph/cluster");
        }
        // Omitting an argument results solely in reporting, and during a
        // build, the answer would be the build's scratch cluster, a name the
        // program cannot use.
        SUBCASE("zelph/cluster without an argument")
        {
            refused("(zelph/cluster)", "zelph/cluster");
        }
        SUBCASE("zelph/cluster-drop")
        {
            refused(R"js((zelph/cluster-drop "other"))js", "zelph/cluster-drop");
        }
        SUBCASE("zelph/import")
        {
            refused("(zelph/import \"" + imported.generic_string() + "\")", "zelph/import");
        }
        SUBCASE("zelph/load")
        {
            refused("(zelph/load \"" + loaded.generic_string() + "\")", "zelph/load");
        }
        SUBCASE("zelph/save")
        {
            refused("(zelph/save \"" + saved.generic_string() + "\")", "zelph/save");
            CHECK_FALSE(std::filesystem::exists(saved));
        }
        SUBCASE("zelph/run")
        {
            refused("(zelph/run)", "zelph/run");
        }
        SUBCASE("zelph/run-once")
        {
            refused("(zelph/run-once)", "zelph/run-once");
        }
        SUBCASE("zelph/run-delta")
        {
            refused("(zelph/run-delta)", "zelph/run-delta");
        }

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "active=" (or (zelph/cluster) "none"))))js");
        CHECK(any_output_contains(collector, "active=exp"));
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));
        collector.clear();
        interactive.process("A is green");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S y O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x y z"});
        collector.clear();
        interactive.process("S n O");
        CHECK(collect_answers(collector).empty()); });
    std::filesystem::remove(imported);
    std::filesystem::remove(loaded);
    std::filesystem::remove(saved);
}

TEST_CASE("zelph/rule: the argument forms cannot query")
{
    // A query resets the block's variable scope, which during the build binds
    // the rule's variables. A query within the argument forms therefore made
    // the consequence's `'X` and `'Y` new variables that display identically
    // to the condition's: the rule was in force and derived `?? q ??` from
    // `a p b`. The query is refused before resetting anything, meaning a
    // program catching the refusal still builds the rule it wrote.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("m is n");
        const std::size_t before = node_count(collector, interactive);
        SUBCASE("the refusal")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (do (zelph/query (zelph/fact 'A "is" 'B)) (zelph/fact 'X "q" 'Y))))js"),
                              doctest::Contains("zelph/query: refused while a rule is being built."));
            CHECK(node_count(collector, interactive) == before);
            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "=>"));

            interactive.process("a p b");
            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a refusal the program catches")
        {
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (do (try (zelph/query (zelph/fact 'A "is" 'B)) ([e] nil)) (zelph/fact 'X "q" 'Y))))js");
            interactive.process("a p b");
            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a q b"});
        } });
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

TEST_CASE("zelph/rule: a failed call leaves no rule its argument forms wrote")
{
    // A rule that the argument forms construct as a `=>` fact adjacent to the
    // rule they generate is not included within it, and becomes active
    // alongside the rule (as demonstrated in the test below). After the call
    // failed, the build kept what the forms had built, and the `=>` fact
    // operated independently as a rule in effect: `c r d` derived `c s d` from
    // a call that had signalled an error. A failed call retains none of the
    // output generated by its forms, just as a wait keeps nothing, and
    // similarly, a failed thunk of zelph/build-rule retains nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        SUBCASE("zelph/rule")
        {
            interactive.process(R"js(%(try (zelph/rule [(zelph/fact 'X "p" 'Y)] (do (zelph/fact (zelph/fact 'A "r" 'B) "=>" (zelph/fact 'A "s" 'B)) (error "outer fails"))) ([e] nil)))js");
        }
        SUBCASE("zelph/build-rule")
        {
            interactive.process(R"js(%(try (zelph/build-rule (fn [] (zelph/fact (zelph/fact 'A "r" 'B) "=>" (zelph/fact 'A "s" 'B)) (error "thunk fails"))) ([e] nil)))js");
        }
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));

        interactive.process("c r d");
        collector.clear();
        interactive.process("S s O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("zelph/rule: an argument that is no node leaves no half-built rule")
{
    // The builder checked a consequence solely at the moment of writing it. If
    // the second consequence was not a node, the condition set and the first
    // `=>` fact remained active: a generator whose ground condition `k is on`
    // had never been marked by a failed call, thus the data met its
    // requirement, and `a p b` brought the inner rule into force. Should the
    // first consequence not be a node, the condition set and its conditions
    // persisted, with `k is on` included as data. Every argument undergoes
    // verification before any writing, and a failed build keeps nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        SUBCASE("the second consequence")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y) (zelph/fact "k" "is" "on")] (zelph/fact (zelph/fact 'A "r" 'B) "=>" (zelph/fact 'A "s" 'B)) "notafact"))js"),
                              doctest::Contains("zelph/rule: consequence at index 1 is not a valid zelph/node"));
        }
        SUBCASE("the first consequence")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact "k" "is" "on") (zelph/fact 'X "p" 'Y)] "notafact"))js"),
                              doctest::Contains("zelph/rule: consequence at index 0 is not a valid zelph/node"));
        }
        // A node generated by a rolled-back build and still referenced by a
        // binding is not a node either. The build has already discarded what
        // it wrote; the call failed with the network's "requested left node
        // ... does not exist" rather than naming the argument.
        SUBCASE("a node of a rolled-back build")
        {
            interactive.process("%(var saved nil)");
            interactive.process(R"js(%(try (zelph/rule [(zelph/fact 'X "m" 'Y)] (do (set saved (zelph/fact "g" "is" "gone")) (error "outer fails"))) ([e] nil)))js");
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y) (zelph/fact "k" "is" "on")] (zelph/fact 'X "q" 'Y) saved))js"),
                              doctest::Contains("zelph/rule: consequence at index 1 is not a valid zelph/node"));
        }
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));
        collector.clear();
        interactive.process("k is O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S ~ conjunction");
        CHECK(collect_answers(collector).empty());

        process_lines(interactive, "a p b\nc r d");
        collector.clear();
        interactive.process("S s O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("zelph/rule*: an argument that is no node leaves no rule")
{
    // zelph/rule* opens no build, thus nothing undid what it had written before
    // it came to an invalid argument. If its second consequence was no node,
    // the rule remained effective with the first consequence. It validates each
    // argument prior to writing anything. The arguments are constructed
    // beforehand, treated as claims, before it runs.
    //
    // An identifier that refers to no node within the graph passed the
    // validation: a node generated by a rolled-back build and still
    // referenced by a Janet binding, or a numeric value. The invocation
    // encountered failure in the network with the message "requested left
    // node ... does not exist" upon reaching that argument. Passed as a
    // consequence, it left the rule in force with the consequences preceding
    // it; as a condition, a condition set that was partially written.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rolled_back = [&]
        {
            interactive.process("%(var saved nil)");
            interactive.process(R"js(%(try (zelph/rule [(zelph/fact 'X "m" 'Y)] (do (set saved (zelph/fact "k" "is" "on")) (error "outer fails"))) ([e] nil)))js");
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "mentioned=" (zelph/mentioned "k" "is" "on"))))js");
            CHECK(any_output_contains(collector, "mentioned=false"));
        };
        const auto refused = [&](const std::string& arguments, const std::string& message)
        {
            interactive.process(R"js(%(def [condition consequence] [(zelph/fact 'X "p" 'Y) (zelph/fact 'X "q" 'Y)]))js");
            const std::size_t before = node_count(collector, interactive);
            CHECK_THROWS_WITH(interactive.process("%(zelph/rule* " + arguments + ")"), doctest::Contains(message));
            CHECK(node_count(collector, interactive) == before);
        };
        SUBCASE("a string")
        {
            refused(R"js([condition] consequence "notafact")js", "zelph/rule: consequence at index 1 is not a valid zelph/node");
        }
        SUBCASE("a node of a rolled-back build")
        {
            rolled_back();
            refused("[condition] consequence saved", "zelph/rule: consequence at index 1 is not a valid zelph/node");
        }
        SUBCASE("a literal number")
        {
            REQUIRE_FALSE(interactive.graph()->exists(123456789));
            refused("[condition] consequence 123456789", "zelph/rule: consequence at index 1 is not a valid zelph/node");
        }
        SUBCASE("a condition of a rolled-back build")
        {
            rolled_back();
            refused("[condition saved] consequence", "zelph/rule: condition at index 1 is not a valid zelph/node");
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));
        collector.clear();
        interactive.process("S ~ conjunction");
        CHECK(collect_answers(collector).empty());

        interactive.process("a p b");
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("zelph/rule: a variable named again after a refused rule is a fresh one")
{
    // A variable symbol assigns a single node within a whole Janet block. Any
    // build that is not preserved discards the variables it generated along
    // with its temporary workspace, yet the block continues to associate those
    // names with the vanished nodes: when the subsequent rule within the same
    // block attempted to reference X again, it received a node that had been
    // deleted, resulting in the block ending with the message "requested left
    // node ... does not exist". A name whose underlying node has disappeared
    // now refers to a new variable, regardless of the manner in which the build
    // was discarded: following a wait, as a duplicate of a rule currently in
    // force, or after an error, which no longer maintains the state established
    // by the build.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a wait")
        {
            interactive.process(R"js(%(do (try (zelph/rule [(zelph/fact 'X "p" 'Y)] (do (ev/sleep 0) (zelph/fact 'X "q" 'Y))) ([e] nil)) (zelph/rule [(zelph/fact 'X "r" 'Y)] (zelph/fact 'X "s" 'Y))))js");
        }
        SUBCASE("a duplicate")
        {
            interactive.process("(X p Y) => (X q Y)");
            interactive.process(R"js(%(do (zelph/dedup-rule (fn [] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y)))) (zelph/rule [(zelph/fact 'X "r" 'Y)] (zelph/fact 'X "s" 'Y))))js");
        }
        SUBCASE("an error")
        {
            interactive.process(R"js(%(do (try (zelph/rule [(zelph/fact 'X "p" 'Y)] (do (zelph/fact 'X "q" 'Y) (error "fails"))) ([e] nil)) (zelph/rule [(zelph/fact 'X "r" 'Y)] (zelph/fact 'X "s" 'Y))))js");
        }
        interactive.process("a r b");
        collector.clear();
        interactive.process("S s O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a s b"}); });
}

TEST_CASE("zelph/rule: a rule is not built inside the thunk of zelph/rule-text")
{
    // zelph/rule-text writes the rule that a statement mentions as a rule
    // build of its own, hence its thunk corresponds to the text of a rule, as
    // the argument forms of zelph/rule are. A rule built there was in force by
    // itself, even though the statement merely mentions it. The refusal occurs
    // before any construction, and the surrounding statement is not written.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/fact "x" "noted" (zelph/rule-text (fn [] (zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "q" 'Y))))))js"),
                          doctest::Contains("a rule cannot be built while another rule is being built"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));

        collector.clear();
        interactive.process("S noted O");
        CHECK(collect_answers(collector).empty()); });
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

TEST_CASE("zelph/rule: no function in its expansion builds a rule past the refusal")
{
    // The expansion held the macro's builder: zelph/rule* devoid of refusal. A
    // program that extracted it from an expansion via macex1 and invoked it
    // within another rule's build wrote a rule of its own in that location,
    // which the outer build preserved, in force by itself. The builder was
    // unable to reject such an invocation: it ran from the same frame as the
    // macro's original call. The expansion currently holds no builder, and the
    // rule is constructed from the value returned by its thunk, within the
    // build context. Every function within an expansion, when invoked inside a
    // build that keeps what it constructs, builds nothing, whether using the
    // arguments the expansion supplies or those of zelph/rule*. The arguments
    // are built beforehand, so the graph must remain unchanged.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(defn cfunctions [form] (cond (cfunction? form) [form] (indexed? form) (mapcat cfunctions form) [])))js");
        interactive.process(R"js(%(def found (cfunctions (macex1 '(zelph/rule [c] k)))))js");
        interactive.process(R"js(%(def conditions [(zelph/fact 'A "r" 'B)]))js");
        interactive.process(R"js(%(def consequence (zelph/fact 'A "s" 'B)))js");
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "found=" (length found))))js");
        CHECK_FALSE(any_output_contains(collector, "found=0"));

        const std::size_t before = node_count(collector, interactive);
        const auto        call   = [&](const std::string& arguments)
        {
            collector.clear();
            interactive.process(R"js(%(each f found (zelph/build-rule (fn [] (zelph/out (string "call: " (try (do (f )js" + arguments + R"js() "built") ([e] e)))) nil))))js");
            CHECK(any_output_contains(collector, "call: "));
            CHECK_FALSE(any_output_contains(collector, "call: built"));
        };
        SUBCASE("a thunk as the expansion passes it")
        {
            call("(fn [] [conditions consequence])");
            CHECK(count_outputs_containing(collector, "call: ") == count_outputs_containing(collector, "call: zelph/rule: a rule cannot be built while another rule is being built"));
        }
        SUBCASE("the arguments of zelph/rule*")
        {
            call("conditions consequence");
        }

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));

        interactive.process("c r d");
        collector.clear();
        interactive.process("S s O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("zelph/rule: a bad argument is reported as before, by the macro and by zelph/rule*")
{
    // The builder now throws an exception instead of panicking. Regarding
    // the macro, the builder operates within the build process, and a panic
    // in that context would bypass the template scope's destructor. The
    // messages it issues remain the same.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::string pattern     = "zelph/rule: condition at index 0 is \"a\", which is not a fact pattern and can never match";
        const std::string consequence = "zelph/rule: consequence at index 1 is not a valid zelph/node";
        SUBCASE("zelph/rule with a condition that is no pattern")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/resolve "a")] (zelph/fact "x" "q" "y")))js"), doctest::Contains(pattern));
        }
        SUBCASE("zelph/rule* with a condition that is no pattern")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule* [(zelph/resolve "a")] (zelph/fact "x" "q" "y")))js"), doctest::Contains(pattern));
        }
        SUBCASE("zelph/rule with a consequence that is no node")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "q" 'Y) "notafact"))js"), doctest::Contains(consequence));
        }
        SUBCASE("zelph/rule* with a consequence that is no node")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/rule* [(zelph/fact 'X "p" 'Y)] (zelph/fact 'X "q" 'Y) "notafact"))js"), doctest::Contains(consequence));
        } });
}

TEST_CASE("zelph/build-rule-parts: a thunk that returns no tuple builds no rule")
{
    // The macro's thunk consistently returns a tuple; a program invoking the
    // function manually might not. Consequently, no components exist to build
    // the rule from, the build process fails with a distinct message, and the
    // content the thunk produced is discarded along with it -- `k is on`
    // included, which would otherwise serve as data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/build-rule-parts (fn [] (zelph/fact 'X "p" 'Y) (zelph/fact "k" "is" "on") @{})))js"),
                          doctest::Contains("zelph/rule: the forms did not return the rule's conditions and consequences"));
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));
        collector.clear();
        interactive.process("k is O");
        CHECK(collect_answers(collector).empty()); });
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

            // The firing's conclusion claims the fact nested in it, just as
            // the identical statement typed does; it formerly remained a
            // pattern.
            collector.clear();
            interactive.process("a p b");
            CHECK(any_deduction_of(collector, "a likes (k is green)"));
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k is green"});
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

// ---------------------------------------------------------------------------
// Janet threads. A thread initiated by a program via `ev/thread`,
// `ev/spawn-thread`, or `ev/do-thread` possesses its own Janet VM, yet invokes
// identical zelph functions through the same engine, with the state underlying a
// statement, a rule build, and the scratch clusters existing only once per
// process. While the main thread remained active, any facts authored by such a
// thread were silently discarded during the main thread's next scratch drop; a
// correct rule was rejected as nested within another, a claim vanished, and the
// main thread's active cluster was reset. Consequently, operations such as
// writing the graph, building a rule, and changing the engine's state are
// disallowed on such a thread; however, reading the graph and using a compiled
// network are permitted (refer to test_neural.cpp for the networks).
// ---------------------------------------------------------------------------

namespace
{
    // A Janet block that evaluates `form` within a thread, then outputs the
    // result it returned, or displays "refused: " followed by the error it
    // triggered. The block remains idle until the thread completes, ensuring
    // the line becomes part of the collector once process() returns.
    std::string on_janet_thread(const std::string& form)
    {
        return R"js(%(do (def ch (ev/thread-chan 1)) (ev/spawn-thread (ev/give ch (try (string "returned: " )js" + form
             + R"js() ([e] (string "refused: " e))))) (zelph/out (ev/take ch))))js";
    }
}

TEST_CASE("zelph/fact: a call from a Janet thread is refused, and nothing is written")
{
    // The refusal occurs before the resolution of any single name,
    // meaning that the nodes associated with the three names are never
    // instantiated.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(on_janet_thread(R"js((zelph/fact "ta" "tp" "tb"))js"));
        CHECK(any_output_contains(collector, "refused: zelph/fact: main thread only"));

        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("zelph/rule: a rule from a Janet thread is refused as main thread only, not as a nested rule")
{
    // While the main thread was situated within `zelph/rule`'s argument
    // forms, a rule authored by a thread was rejected due to "a rule cannot be
    // built while another rule is being built" -- the flag that marks a build
    // is present only once per process, and the thread's rule is not contained
    // within any other structure. It is now refused for what it is, and
    // the main thread's rule proceeds to be constructed as if the thread had
    // never existed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A single block: the thread remains pending until the main thread
        // enters the argument forms, and the forms stay blocked, refraining
        // from relinquishing control to the event loop, until the thread
        // receives its answer.
        interactive.process(R"js(%(do (def go (ev/thread-chan 1)) (def done (ev/thread-chan 1)) (def res (ev/thread-chan 1)) (ev/thread (fn [] (while (= 0 (ev/count go)) (os/sleep 0.01)) (ev/give res (try (do (zelph/rule [(zelph/fact 'X "p2" 'Y)] (zelph/fact 'X "q2" 'Y)) "built") ([e] (string "refused: " e)))) (ev/give done true)) nil :n) (zelph/rule [(zelph/fact 'X "p" 'Y)] (do (ev/give go true) (while (= 0 (ev/count done)) (os/sleep 0.01)) (zelph/fact 'X "q" 'Y))) (zelph/out (ev/take res))))js");
        CHECK(any_output_contains(collector, "refused: zelph/rule: main thread only"));
        CHECK_FALSE(any_output_contains(collector, "while another rule is being built"));

        process_lines(interactive, "a p b\na p2 b");
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a q b"});
        collector.clear();
        interactive.process("S q2 O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("zelph/collection: a call from a Janet thread is refused")
{
    // A collection constructed by a thread while the main thread wrote a
    // rule used to be written as a value, since the rule's scope is tied to
    // the thread that opens it. That scope (Zelph::TemplateScope, pinned in
    // test_recipe_ids.cpp) can now no longer be reached from a Janet thread
    // in any way: attempting to build anything on such a thread is refused.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(on_janet_thread(R"js((zelph/collection "q1"))js"));
        CHECK(any_output_contains(collector, "refused: zelph/collection: main thread only"));

        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("zelph/exists: a question from a Janet thread is answered")
{
    // Reading the graph is not rejected on a thread.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");

        collector.clear();
        interactive.process(on_janet_thread(R"js((string (zelph/exists "a" "p" "b") " " (zelph/exists "a" "p" "c")))js"));
        CHECK(any_output_contains(collector, "returned: true false")); });
}

TEST_CASE("zelph/resolve: a Janet thread looks a name up, and creating one is refused")
{
    // A thread responsible for evaluating a compiled network resolves the
    // names of its inputs (test_neural.cpp, "a network can be
    // evaluated while another thread trains it"). If a name already
    // corresponds to a node, it responds as it does on the main thread; if a
    // name requires a new node, it constitutes a write.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("known p b");
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(on_janet_thread(R"js((zelph/name (zelph/resolve "known")))js"));
        CHECK(any_output_contains(collector, "returned: known"));

        // The refusal names the name: the call performs a lookup that yielded
        // no result, something a program can tell apart from a call that has
        // no business on a thread at all ("zelph/resolve: main thread only",
        // as it previously stated).
        collector.clear();
        interactive.process(on_janet_thread(R"js((zelph/resolve "unheard-of"))js"));
        CHECK(any_output_contains(collector, "refused: zelph/resolve: 'unheard-of' has no node, and creating one is main thread only."));

        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("zelph/resolve: on a Janet thread the empty name gets the main thread's error")
{
    // No node may hold the empty name, thus the call constitutes an error on
    // every thread, and it is the identical error: refusing it as a write
    // that only the main thread is permitted to perform would unfairly
    // attribute blame to the thread for a name that is invalid everywhere.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(R"js(%(zelph/out (try (string "returned: " (zelph/resolve "")) ([e] (string "refused: " e)))))js");
        const std::string on_main = last_out_text(collector);
        REQUIRE(on_main.rfind("refused: ", 0) == 0);

        collector.clear();
        interactive.process(on_janet_thread(R"js((zelph/resolve ""))js"));
        CHECK(last_out_text(collector) == on_main); });
}

TEST_CASE("zelph/resolve: on a Janet thread a variable's name is not the node of that name")
{
    // The names associated with the variables in a typed rule are preserved
    // for display, and the name lookup process for commands finds them.
    // `zelph/resolve` answers with an atom and creates one when only a
    // variable carries the name; a thread that is unable to produce it is
    // rejected instead of being given the variable.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(V p W) => (V r W)");
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(on_janet_thread(R"js((zelph/resolve "V"))js"));
        CHECK(any_output_contains(collector, "refused: zelph/resolve: 'V' has no node, and creating one is main thread only."));

        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("Janet threads: every function that writes the graph or changes the engine is refused")
{
    // One call of each function, from a single thread; the line lists every
    // call that was NOT refused as main thread only, specifying what was
    // raised in its place. Arguments are irrelevant: the rejection occurs
    // before their interpretation. `zelph/resolve` looks a name up on a
    // thread, so its refusal names the name lacking a node, and the entry
    // carries that text in place of "zelph/resolve: main thread only".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(R"js(%(do (def ch (ev/thread-chan 1)) (ev/spawn-thread (def accepted @[]) (each [name f refusal] [["zelph/fact" (fn [] (zelph/fact "ta" "tp" "tb"))] ["zelph/refute" (fn [] (zelph/refute "ta" "tp" "tc"))] ["zelph/negate" (fn [] (zelph/negate nil))] ["zelph/path-guard" (fn [] (zelph/path-guard "ta" "tb"))] ["zelph/list" (fn [] (zelph/list "ta" "tb"))] ["zelph/list-chars" (fn [] (zelph/list-chars "12"))] ["zelph/set" (fn [] (zelph/set "ta"))] ["zelph/collection" (fn [] (zelph/collection "ta"))] ["zelph/conjunction" (fn [] (zelph/conjunction nil nil))] ["zelph/resolve" (fn [] (zelph/resolve "never-named")) "zelph/resolve: 'never-named' has no node, and creating one is main thread only"] ["zelph/var" (fn [] (zelph/var))] ["zelph/query" (fn [] (zelph/query nil))] ["zelph/rule*" (fn [] (zelph/rule* [] nil))] ["zelph/rule" (fn [] (zelph/rule [] nil))] ["zelph/build-rule" (fn [] (zelph/build-rule (fn [] nil)))] ["zelph/dedup-rule" (fn [] (zelph/dedup-rule (fn [] nil)))] ["zelph/rule-text" (fn [] (zelph/rule-text (fn [] nil)))] ["zelph/register-keyword" (fn [] (zelph/register-keyword "tkw" (fn [x] x)))] ["zelph/approx" (fn [] (zelph/approx nil "tnet"))] ["zelph/path" (fn [] (zelph/path nil "one-or-more"))] ["zelph/nn-connect" (fn [] (zelph/nn-connect "ta" "tb"))] ["zelph/set-weight" (fn [] (zelph/set-weight "ta" "tb" 1))] ["zelph/nn-compile" (fn [] (zelph/nn-compile [nil nil]))] ["zelph/nn-connect-layers" (fn [] (zelph/nn-connect-layers "ta" "tb"))] ["zelph/set-number-digits" (fn [] (zelph/set-number-digits []))] ["zelph/register-display-scheme" (fn [] (zelph/register-display-scheme "ts" "(" ")"))] ["zelph/set-infix-display" (fn [] (zelph/set-infix-display "ts" []))] ["zelph/set-application-display" (fn [] (zelph/set-application-display "ts" []))] ["zelph/no-selffact-sugar" (fn [] (zelph/no-selffact-sugar "tp"))] ["zelph/import" (fn [] (zelph/import "tfile"))] ["zelph/save" (fn [] (zelph/save "tfile.bin"))] ["zelph/load" (fn [] (zelph/load "tfile.bin"))] ["zelph/run" (fn [] (zelph/run))] ["zelph/run-once" (fn [] (zelph/run-once))] ["zelph/run-delta" (fn [] (zelph/run-delta))] ["zelph/cluster" (fn [] (zelph/cluster "tc"))] ["zelph/cluster-drop" (fn [] (zelph/cluster-drop "tc"))] ["zelph/clusters" (fn [] (zelph/clusters))]] (try (do (f) (array/push accepted name)) ([e] (unless (string/find (or refusal (string name ": main thread only")) (string e)) (array/push accepted (string name " -> " e)))))) (ev/give ch (string "accepted: [" (string/join accepted ", ") "]"))) (zelph/out (ev/take ch))))js");
        CHECK(last_out_text(collector) == "accepted: []");

        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("zelph/rule: argument forms that wait for the event loop are refused, and nothing of the rule remains")
{
    // The argument forms execute inside the rule's build, via a C call, with
    // the template scope open. A form that waits for the event loop, like
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
