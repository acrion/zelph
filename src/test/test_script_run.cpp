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
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

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

TEST_CASE("zelph/run: an argument other than :quiet and :derived is refused")
{
    // This case was "zelph/run: an argument other than :quiet is refused",
    // and previously "zelph/run: arity is fixed at zero". It pinned that
    // :quiet, a run that prints nothing and returns what it met, was the
    // sole accepted argument, with a message naming it. :derived now joins
    // it, adding the facts the run derived to what it returns, so the
    // message names both. A second :quiet, previously rejected due to arity,
    // is now rejected by the message, and :derived alone is refused, since
    // a run that prints returns nil and would gather facts for nobody.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        for (const std::string name : {"zelph/run", "zelph/run-once", "zelph/run-delta"})
        {
            CHECK_THROWS_WITH(interactive.process("%(" + name + " \"unexpected\")"), doctest::Contains(name + ": the arguments are :quiet and :derived"));
            CHECK_THROWS_WITH(interactive.process("%(" + name + " :loud)"), doctest::Contains(name + ": the arguments are :quiet and :derived"));
            CHECK_THROWS_WITH(interactive.process("%(" + name + " :quiet :quiet)"), doctest::Contains(name + ": the arguments are :quiet and :derived"));
            CHECK_THROWS_WITH(interactive.process("%(" + name + " :quiet :loud)"), doctest::Contains(name + ": the arguments are :quiet and :derived"));
            CHECK_THROWS_WITH(interactive.process("%(" + name + " :derived)"), doctest::Contains(name + ": :derived needs :quiet"));
            CHECK_THROWS(interactive.process("%(" + name + " :quiet :derived :quiet)"));
            CHECK_NOTHROW(interactive.process("%(" + name + " :quiet :derived)"));
            CHECK_NOTHROW(interactive.process("%(" + name + " :derived :quiet)"));
        } });
}

// ---------------------------------------------------------------------------
// `zelph/run :quiet`: a run that prints nothing and returns what it met.
//
// A front end that translates its own language into zelph reports what a
// run detected within that language. A loud run printed it in zelph
// syntax: the banner, each deduction, the `!` line corresponding to a
// contradiction, and the list generated in check mode. A quiet run outputs
// nothing and instead returns the same results as data: {:contradictions C
// :naf-final N}. Each case described below pins a specific component of
// that return, or a line that is no longer printed.
// ---------------------------------------------------------------------------

namespace
{
    // Prints what a quiet run returned, presenting each part on a separate
    // line, enabling a test to retrieve it from the collector: the totals,
    // followed by a line for each contradiction and another for each
    // defeated negation. A binding appears as the names of the variable
    // and of its value, listed in sorted order.
    constexpr const char* kShowRun =
        R"js(%(defn show-run [tag r] (zelph/out (string tag " contradictions=" (length (r :contradictions)) " naf-final=" (length (r :naf-final)))) (each e (r :contradictions) (zelph/out (string tag " record=" (e :record) " conclusion=" (if (e :conclusion) (string (e :conclusion)) "none") " conditions=" (string/join (map string (e :conditions)) " | ") " bindings=" (string/join (sorted (map |(string (zelph/name ($ 0)) "=" (zelph/name ($ 1))) (pairs (e :bindings)))) ",") (if (e :refused) (string " refused=" (e :refused)) "")))) (each n (r :naf-final) (zelph/out (string tag " defeated " (n :fact) " by " (n :rule))))))js";

    // Prints the :derived section of the result from a quiet run, with each
    // derived fact shown on a separate line, ordered as derived: the fact
    // itself, followed by its rule, the premises, and the absent patterns
    // (each sorted by their textual representation), along with the
    // bindings formatted as kShowRun would display them.
    constexpr const char* kShowDerived =
        R"js(%(defn show-derived [tag r] (zelph/out (string tag " derived=" (length (r :derived)))) (each e (r :derived) (zelph/out (string tag " fact=" (e :fact) " rule=" (e :rule) " premises=" (string/join (sorted (map string (e :premises))) " | ") " absent=" (string/join (sorted (map string (e :absent))) " | ") " bindings=" (string/join (sorted (map |(string (zelph/name ($ 0)) "=" (zelph/name ($ 1))) (pairs (e :bindings)))) ","))))))js";

    // cr4 of the investigation that led to the quiet run: the rule's
    // consequence is a fact that the graph recognizes as incorrect, thus
    // the run meets a contradiction involving two matched conditions, which
    // a record can name. Auto-run is off, making the quiet run the first one
    // to meet it.
    void define_sleeps(const zelph::console::Interactive& interactive)
    {
        interactive.process(".auto-run");
        interactive.process(kShowRun);
        interactive.process("((X waits nil), (X ~ man)) => (X sleeps nil)");
        interactive.process("¬(john sleeps nil)");
        interactive.process("john ~ man");
        interactive.process("john waits nil");
    }

    // A conclusion drawn under a negation, followed by a later fact that
    // defeats it: the loud run derives `bill is happy` under ¬(bill h1
    // nil), and subsequently `bill h1 nil` is stated, with auto-run off.
    // The next run that keeps negation records finds the fact without
    // support.
    void define_happy(const zelph::console::Interactive& interactive)
    {
        interactive.process(".auto-run");
        interactive.process(kShowRun);
        interactive.process(kReport);
        interactive.process("(M ~ man, ¬(M h1 nil)) => (M is happy)");
        interactive.process("bill ~ man");
        interactive.process(".run");
        interactive.process("bill h1 nil");
    }
} // namespace

TEST_CASE("zelph/run: :quiet derives without a word")
{
    // A loud run printed its banner, the deductions and "Ready.", while a
    // silent one still noted the deductions it hid and marked the prompt;
    // within a front end's output, each of these constitutes zelph syntax
    // that the front end did not write. A quiet run prints nothing, treats
    // nothing as hidden, and still derives. The three deduction modes
    // differ in what a loud run prints, hence each is checked.
    const auto quiet_run = [](auto& collector, auto& interactive)
    {
        interactive.process(kReport);
        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "mortal")))js");
        collector.clear();
        interactive.process(R"js(%(do (zelph/run :quiet) nil))js");
        CHECK(collector.events().empty());
        CHECK(interactive.graph()->deductions_withheld() == 0);
        CHECK(interactive.prompt_text().find('+') == std::string::npos);

        interactive.process(R"js(%(report "after" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "after=yes"));
    };
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        SUBCASE("in the default deduction mode") { quiet_run(collector, interactive); }
        SUBCASE("with .deductions all") { interactive.process(".deductions all"); quiet_run(collector, interactive); }
        SUBCASE("with .deductions off, where a silent run notes what it hid") { interactive.process(".deductions off"); quiet_run(collector, interactive); } });
}

TEST_CASE("zelph/run: :quiet :derived returns each derived fact with its rule and premises")
{
    // A run that prints displays every deduction alongside its premises
    // following `⇐`, while a quiet run revealed none of these details: only
    // .run-export recorded them, saving them to a file. Using :derived, a
    // quiet run returns them as data, enabling a front end to communicate
    // in its own language what was concluded from what.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kShowDerived);
        interactive.process("(X ~ man) => (X ~ mortal)");
        interactive.process("(X ~ mortal, ¬(X ~ god)) => (X ~ doomed)");
        interactive.process("socrates ~ man");

        SUBCASE("each fact with its rule, the facts its conditions matched, and the bindings")
        {
            collector.clear();
            interactive.process(R"js(%(show-derived "d" (zelph/run :quiet :derived)))js");
            CHECK(any_output_contains(collector, "d derived=2"));
            CHECK(any_output_contains(collector, "d fact=socrates ~ mortal rule=(X ~ man) => (X ~ mortal) premises=socrates ~ man absent= bindings=X=socrates"));
            // The condition that was negated matched no fact: it is named
            // among the absent patterns, with the variable that the bindings
            // resolve.
            CHECK(any_output_contains(collector, "d fact=socrates ~ doomed rule=((X ~ mortal), ¬(X ~ god)) => (X ~ doomed) premises=socrates ~ mortal absent=X ~ god bindings=X=socrates"));
        }
        SUBCASE("a rule with two consequences returns each fact it wrote")
        {
            // The consequences of a rule consist of the objects in a single
            // `=>` fact, serving as the mechanism through which a front end
            // writes a rule having multiple heads: each fact written by a
            // single firing constitutes an independent entry, sharing the
            // same rule, premises, and bindings.
            interactive.process("(X ~ man, X likes Y) => (X ~ lover) (Y ~ loved)");
            interactive.process("socrates likes plato");
            collector.clear();
            interactive.process(R"js(%(let [heads (filter |(string/find "lov" (string ($ :fact))) ((zelph/run :quiet :derived) :derived)) [a b] heads] (zelph/out (string "HEADS=" (string/join (sorted (map |(string ($ :fact)) heads)) ",") " premises=" (string/join (sorted (map string (a :premises))) " | ") " same-rule=" (= (a :rule) (b :rule)) " same-premises=" (= (a :premises) (b :premises)) " same-bindings=" (deep= (a :bindings) (b :bindings))))))js");
            CHECK(any_output_contains(collector, "HEADS=plato ~ loved,socrates ~ lover premises=socrates likes plato | socrates ~ man same-rule=true same-premises=true same-bindings=true"));
        }
        SUBCASE("in the order derived")
        {
            // socrates ~ doomed rests on socrates ~ mortal, hence it must
            // follow it, regardless of the evaluation order.
            collector.clear();
            interactive.process(R"js(%(zelph/out (string/join (map |(string ($ :fact)) ((zelph/run :quiet :derived) :derived)) " then ")))js");
            CHECK(any_output_contains(collector, "socrates ~ mortal then socrates ~ doomed"));
        }
        SUBCASE("a run-delta returns what it derived from the new facts only")
        {
            interactive.process(R"js(%(do (zelph/run :quiet) nil))js");
            interactive.process("plato ~ man");
            collector.clear();
            interactive.process(R"js(%(show-derived "d" (zelph/run-delta :quiet :derived)))js");
            CHECK(any_output_contains(collector, "d derived=2"));
            CHECK(any_output_contains(collector, "d fact=plato ~ mortal"));
            CHECK(any_output_contains(collector, "d fact=plato ~ doomed"));
            CHECK_FALSE(any_output_contains(collector, "d fact=socrates"));
        }
        SUBCASE("a later run returns only what it derived itself")
        {
            // At the prompt, `.run quiet derived` retains what it derived
            // for a caller that never comes; the next run must refrain from
            // passing it along.
            interactive.process(".run quiet derived");
            interactive.process("plato ~ man");
            collector.clear();
            interactive.process(R"js(%(show-derived "d" (zelph/run-delta :quiet :derived)))js");
            CHECK(any_output_contains(collector, "d derived=2"));
            CHECK_FALSE(any_output_contains(collector, "d fact=socrates"));
        }
        SUBCASE("a single pass returns what it derived")
        {
            collector.clear();
            interactive.process(R"js(%(show-derived "d" (zelph/run-once :quiet :derived)))js");
            CHECK(any_output_contains(collector, "d fact=socrates ~ mortal"));
        }
        SUBCASE("without :derived the struct is the one :quiet returns")
        {
            collector.clear();
            interactive.process(R"js(%(zelph/out (string/format "keys=%j" (sorted (keys (zelph/run :quiet))))))js");
            CHECK(any_output_contains(collector, "keys=@[:contradictions :naf-final]"));
        } });
}

TEST_CASE("zelph/run: :quiet :derived names a premise without creating one")
{
    // A condition matches a fact that carries more objects than the
    // condition names: (X p Y) binds Y to b and to c within `a p b c`. The
    // premise is that wider fact, retrieved by a lookup; instantiating the
    // condition under those bindings, as a contradiction's record does,
    // would have created `a p b` and `a p c`, facts that no one asserted,
    // even as the data was being read.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kShowDerived);
        interactive.process("(X p Y) => (X q Y)");
        interactive.process("a p b c");
        collector.clear();
        interactive.process(R"js(%(show-derived "d" (zelph/run :quiet :derived)))js");
        CHECK(any_output_contains(collector, "d derived=2"));
        CHECK(any_output_contains(collector, "d fact=a q b rule=(X p Y) => (X q Y) premises=a p b c absent= bindings=X=a,Y=b"));
        CHECK(any_output_contains(collector, "d fact=a q c rule=(X p Y) => (X q Y) premises=a p b c absent= bindings=X=a,Y=c"));

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "NARROW-" (zelph/exists "a" "p" "b" :exact) "-" (zelph/exists "a" "p" "c" :exact))))js");
        CHECK(any_output_contains(collector, "NARROW-false-false")); });
}

TEST_CASE("zelph/run: :quiet returns a contradiction it records")
{
    // The run writes the contradiction's record and declares it via :new:
    // a later loud run remains silent on the matter, just as after a loud
    // run that recorded it. Nothing of it appears in the output, neither
    // the `!` line nor the message "Found one or more contradictions!",
    // and the entry names the facts the rule matched, the refuted fact
    // that the consequence would have asserted, and the binding.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        define_sleeps(interactive);
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK_FALSE(any_event_contains(collector, "⇐"));
        CHECK_FALSE(has_contradiction(collector));
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=new conclusion=¬(john sleeps nil) conditions="));
        CHECK(any_output_contains(collector, "john ~ man"));
        CHECK(any_output_contains(collector, "john waits nil"));
        CHECK(any_output_contains(collector, "bindings=X=john"));

        collector.clear();
        interactive.process(".run");
        CHECK(any_event_contains(collector, "(1 already recorded in this network)"));
        CHECK_FALSE(any_event_contains(collector, "⇐")); });
}

TEST_CASE("zelph/run: :quiet returns a contradiction a record already holds")
{
    // A record that already holds the contradiction keeps a loud run from
    // announcing it again, and a front end that has observed only the
    // loud output cannot distinguish this contradiction from none. The
    // quiet run returns it, with :existing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        define_sleeps(interactive);
        interactive.process(".run");
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=existing conclusion=¬(john sleeps nil) conditions=")); });
}

TEST_CASE("zelph/run: :quiet returns a contradiction it cannot record")
{
    // When records are disabled, nothing keeps a contradiction: the quiet
    // run returns it with :none, while still naming the facts that its
    // conditions matched, which a loud run could not communicate except
    // through printing them.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".contradiction-records off");
        define_sleeps(interactive);
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=none conclusion=¬(john sleeps nil) conditions="));
        CHECK(any_output_contains(collector, "john ~ man"));
        CHECK(any_output_contains(collector, "john waits nil"));

        // Nothing was recorded, thus the next run meets it once more.
        collector.clear();
        interactive.process(R"js(%(show-run "again" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "again record=none")); });
}

TEST_CASE("zelph/run: :quiet returns a contradiction once however often the run meets it")
{
    // A contradiction without a record is rediscovered on every pass:
    // check mode's classic pass detects it after the seeded pass, and
    // classic evaluation once per iteration of its fixpoint, which the
    // chain below sustains (x15 of the investigation). The quiet run
    // returns it only once. Records are disabled, ensuring no pass finds
    // the contradiction as previously recorded; the entry still names
    // the fact that the rule's single condition matched.
    const auto met_once = [](auto& collector, auto& interactive)
    {
        interactive.process(".contradiction-records off");
        interactive.process(kShowRun);
        process_lines(interactive, R"(
(X ~ man) => !
(X ~ a) => (X ~ b)
(X ~ b) => (X ~ c)
(X ~ c) => (X ~ d)
john ~ man
k ~ a
)");
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=none conclusion=! conditions=john ~ man bindings=X=john"));
    };
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        SUBCASE("check mode, a seeded and a classic pass") { met_once(collector, interactive); }
        SUBCASE("classic evaluation, a pass per iteration") { interactive.process(".semi-naive off"); met_once(collector, interactive); } });
}

// The entry of a quiet run names the facts a single condition matched,
// through the look-up the record employs. Constructed via fact(), the
// refuted fact a marking names was refused as being known to be incorrect,
// resulting in the quiet run failing with that error, while a loud run instead
// reported the contradiction: a rule with one condition was not recorded at
// that time, and only a quiet run had collected the fact its condition
// matched. A loud run now performs the same look-up to record it as well.
TEST_CASE("zelph/run: :quiet names the marking of a refuted fact that a single condition matched")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kShowRun);
        interactive.process("¬(t p b)");
        std::string rule;
        std::string conclusion;
        SUBCASE("a consequence refuted before")
        {
            rule       = "((X p b) ~ refuted) => (X p b)";
            conclusion = "¬(t p b)";
        }
        SUBCASE("a denial")
        {
            rule       = "((X p b) ~ refuted) => !";
            conclusion = "!";
        }
        interactive.process(rule);
        collector.clear();
        CHECK_NOTHROW(interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js"));
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=new conclusion=" + conclusion + " conditions=(t p b) ~ refuted bindings=X=t"));

        collector.clear();
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_FALSE(has_contradiction(collector));
        CHECK(any_event_contains(collector, "1 already recorded in this network"));
        CHECK_THROWS_WITH(interactive.process("t p b"), doctest::Contains("this fact is known to be wrong")); });
}

TEST_CASE("zelph/run: :quiet says why the engine refused to build a consequence")
{
    // A consequence the engine chooses not to build -- specifically, the
    // extension of a set constant -- is reported as a contradiction, and a
    // loud run details the cause on a "refused:" line. In contrast, the quiet
    // run holds no fact to name as the conclusion, resulting in the entry
    // being assigned none and instead bearing the reason. It is recorded just
    // like any other contradiction, over the fact the rule's single
    // condition matched.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kShowRun);
        process_lines(interactive, R"(
z rel {a b}
q p r
(X p Y) => (X in {a b})
)");
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=new conclusion=none conditions=q p r bindings=X=q,Y=r refused=a set constant cannot be extended"));
        CHECK_FALSE(any_event_contains(collector, "└─ refused")); });
}

TEST_CASE("zelph/run: :quiet names the claimed fact that a derived refutation meets")
{
    // A derived refutation that meets a claimed fact constitutes a
    // contradiction (refer to "refuted marking: a derived one meeting a
    // claimed fact is a contradiction"). Since nothing is written, the
    // consequence leaves no fact of its own; the quiet run names the
    // claimed fact it met as the conclusion, just as it names the refuted
    // fact that a consequence would have asserted. The record holds that
    // fact beside the instance of the rule's single condition, enabling the
    // second quiet run to detect it as :existing, while :conditions lists
    // the condition alone.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kShowRun);
        interactive.process("(X ~ person) => ((X sleeps nil) ~ refuted)");
        interactive.process("mary sleeps nil");
        interactive.process("mary ~ person");
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK_FALSE(has_contradiction(collector));
        CHECK(any_output_contains(collector, "q contradictions=1 naf-final=0"));
        CHECK(any_output_contains(collector, "q record=new conclusion=mary sleeps nil conditions=mary ~ person bindings=X=mary"));

        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "q record=existing conclusion=mary sleeps nil conditions=mary ~ person")); });
}

TEST_CASE("zelph/run: :quiet leaves the report of a condition it will not evaluate to the next run that prints")
{
    // A condition that the engine does not evaluate is reported, typically
    // only once per session: a negated guard, a path condition having both
    // ends free, a net lacking layers. During a quiet run, none of these
    // reports are printed and they are not counted as made, meaning the
    // next run that prints outputs them.
    const auto deferred = [](auto& collector, auto& interactive, const std::string& rule, const std::string& report)
    {
        interactive.process(rule);
        collector.clear();
        interactive.process(R"js(%(do (zelph/run :quiet) nil))js");
        CHECK(collector.events().empty());
        interactive.process(".run");
        CHECK(any_event_contains(collector, report));
    };
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        process_lines(interactive, R"(
a p b
a P279 b
s1 relG o1
)");
        SUBCASE("a negated guard") { deferred(collector, interactive, "(X p Y, ¬(X != Y)) => (X same Y)", "Error: ¬ cannot be applied to \"!=\""); }
        SUBCASE("a path condition with both ends free") { deferred(collector, interactive, "(S P279⁺ T) => (S above T)", "Error: a path condition needs at least one bound end"); }
        SUBCASE("a neural condition without a net") { deferred(collector, interactive, "(A relG B, ≈gnet(A relG B)) => (A verifiedG B)", "Neural condition: net 'gnet' has no nn-layers definition"); } });
}

TEST_CASE("zelph/run: :quiet in check mode returns the negation check as data and prints nothing")
{
    // In check mode, the test binary's default setting, an exception is
    // raised once a fact rests on a negation whose pattern has come to
    // hold, after the list is printed. A quiet run does neither and returns
    // the list as :naf-final, with one {:fact :rule} entry for each fact.
    // The fact remains, just as it does after a loud run that raised.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        define_happy(interactive);
        collector.clear();
        SUBCASE("with :quiet")
        {
            CHECK_NOTHROW(interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js"));
            CHECK_FALSE(any_event_contains(collector, "Negation check"));
            CHECK(any_output_contains(collector, "q contradictions=0 naf-final=1"));
            CHECK(any_output_contains(collector, "q defeated bill is happy by (¬(M h1 nil), (M ~ man)) => (M is happy)"));
            interactive.process(R"js(%(report "kept" "bill" "is" "happy"))js");
            CHECK(any_output_contains(collector, "kept=yes"));

            // The check drops the records it reported, thus a second run
            // returns nothing.
            collector.clear();
            interactive.process(R"js(%(show-run "again" (zelph/run :quiet)))js");
            CHECK(any_output_contains(collector, "again contradictions=0 naf-final=0"));
        }
        SUBCASE("without :quiet the run still raises")
        {
            CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/run))js"), doctest::Contains("Negation check: 1 fact(s)"));
            CHECK(any_output_contains(collector, "bill is happy  (by"));
        } });
}

TEST_CASE("zelph/run: :quiet returns what .negation-records finds outside check mode")
{
    // .semi-naive on: in check mode, which this binary employs as
    // standard, records are still preserved, and this scenario concerns
    // the switch that maintains them outside check mode. With
    // .negation-records enabled, the identical list fills, the run issues
    // a warning rather than throwing an exception, and (zelph/defeated)
    // returns the same pair subsequently.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        interactive.process(".negation-records on");
        define_happy(interactive);
        collector.clear();
        interactive.process(R"js(%(show-run "q" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "q contradictions=0 naf-final=1"));
        CHECK(any_output_contains(collector, "q defeated bill is happy by (¬(M h1 nil), (M ~ man)) => (M is happy)"));
        interactive.process(R"js(%(zelph/out (string "defeated " (length (zelph/defeated)) " " (first (first (zelph/defeated))))))js");
        CHECK(any_output_contains(collector, "defeated 1 bill is happy"));

        collector.clear();
        interactive.process(R"js(%(show-run "again" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "again contradictions=0 naf-final=0")); });
}

TEST_CASE("zelph/run: :quiet prints no warning for a fact that .negation-records finds")
{
    // .semi-naive is enabled, just as in the prior example. The alert
    // produced by .negation-records adheres to zelph syntax as well, and a
    // quiet run ensures it does not appear in the output.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        interactive.process(".negation-records on");
        define_happy(interactive);
        collector.clear();
        interactive.process(R"js(%(do (zelph/run :quiet) nil))js");
        CHECK_FALSE(any_event_contains(collector, "no longer justified"));
        CHECK(collector.events().empty()); });
}

TEST_CASE("zelph/run-once: :quiet never re-tests the negation records")
{
    // .semi-naive on, as previously described. A single pass reaches no
    // fixpoint, hence it never re-tests any record, and its :naf-final is
    // always empty. The record persists into the next full run, which finds
    // the fact.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        interactive.process(".negation-records on");
        define_happy(interactive);
        collector.clear();
        interactive.process(R"js(%(show-run "once" (zelph/run-once :quiet)))js");
        CHECK(any_output_contains(collector, "once contradictions=0 naf-final=0"));
        interactive.process(R"js(%(show-run "full" (zelph/run :quiet)))js");
        CHECK(any_output_contains(collector, "full contradictions=0 naf-final=1")); });
}

TEST_CASE("commands: .run quiet prints nothing, and an unknown argument is refused")
{
    // The commands used to disregard their arguments: `.run quiet` executed
    // a loud pass, as did a misspelling. They now accept `quiet`, matching
    // the behaviour of (zelph/run :quiet), and reject any other argument.
    const auto prints_nothing = [](auto& collector, auto& interactive, const std::string& command)
    {
        interactive.process(kReport);
        interactive.process("socrates ~ human");
        interactive.process("(X ~ human) => (X ~ mortal)");
        collector.clear();
        interactive.process(command);
        CHECK(collector.events().empty());
        interactive.process(R"js(%(report "after" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "after=yes"));
    };
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        SUBCASE(".run quiet") { prints_nothing(collector, interactive, ".run quiet"); }
        SUBCASE(".run-once quiet") { prints_nothing(collector, interactive, ".run-once quiet"); }
        SUBCASE(".run-delta quiet") { prints_nothing(collector, interactive, ".run-delta quiet"); }
        // `derived` is what (zelph/run :quiet :derived) forwards: the same
        // quiet run, which also retains the facts it derives.
        SUBCASE(".run quiet derived") { prints_nothing(collector, interactive, ".run quiet derived"); }
        SUBCASE(".run-delta derived quiet") { prints_nothing(collector, interactive, ".run-delta derived quiet"); }
        SUBCASE("an unknown argument")
        {
            CHECK_THROWS_WITH(interactive.process(".run loud"), doctest::Contains("Usage: .run [quiet]"));
            CHECK_THROWS_WITH(interactive.process(".run-once loud"), doctest::Contains("Usage: .run-once [quiet]"));
            CHECK_THROWS_WITH(interactive.process(".run-delta loud"), doctest::Contains("Usage: .run-delta [quiet]"));
            CHECK_THROWS_WITH(interactive.process(".run quiet quiet"), doctest::Contains("Usage: .run [quiet]"));
            CHECK_THROWS_WITH(interactive.process(".run quiet derived derived"), doctest::Contains("Usage: .run [quiet] [derived]"));
        }
        SUBCASE("derived without quiet")
        {
            CHECK_THROWS_WITH(interactive.process(".run derived"), doctest::Contains(".run: derived needs quiet"));
            CHECK_THROWS_WITH(interactive.process(".run-once derived"), doctest::Contains(".run-once: derived needs quiet"));
            CHECK_THROWS_WITH(interactive.process(".run-delta derived"), doctest::Contains(".run-delta: derived needs quiet"));
        } });
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

// A `.run-once` that derives something halts before reaching the fixpoint -- it
// issues a notice that further iterations were suppressed -- and yet it left
// the record of additions valid: the subsequent `.run-delta` was seeded from
// what arrived afterwards and never came back to what the single pass had left
// undone. A rule featuring a negated condition was spared as long as it took a
// classic pass at each level of negation; seeded at its own level, it missed
// the instance too. This kind of pass now leaves no valid record, and the next
// incremental request takes a full pass.
TEST_CASE("run-delta: falls back to a full pass after a .run-once that stopped short")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kReport);

        const auto after_run_once = [&](const std::string& rules)
        {
            process_lines(interactive, rules + "\njohn ~ man\n.run-once\nx likes y");
            collector.clear();
            interactive.process(".run-delta");
            CHECK(any_event_contains(collector, "Incremental run not applicable"));
        };

        // The single pass leaves the end of the chain undone, something that a
        // run seeded from `x likes y` by itself would never be able to derive.
        SUBCASE("a chain the single pass did not finish")
        {
            after_run_once("(X ~ c2) => (X ~ c3)\n(X ~ c1) => (X ~ c2)\n(X ~ man) => (X ~ c1)");
            interactive.process(R"js(%(report "john" "john" "~" "c3"))js");
            CHECK(any_output_contains(collector, "john=yes"));
        }
        // In this case, the single pass results in the NAF rule lacking an
        // instance: `john ~ mortal` has not yet come into existence at the time
        // the rule is executed. Seeded from `x likes y`, the next run will
        // never meet john, whereas a classic pass operating at the negation
        // level previously succeeded in locating him.
        SUBCASE("a NAF rule the single pass ran too early")
        {
            after_run_once("(X ~ man) => (X ~ mortal)\n((X ~ mortal), ¬(X waits nil)) => (X ~ impatient)");
            interactive.process(R"js(%(report "john" "john" "~" "impatient"))js");
            CHECK(any_output_contains(collector, "john=yes"));
        }
        // A single pass yielding no new facts has found the graph at its
        // fixpoint, and the record stays valid.
        SUBCASE("a single pass that derived nothing keeps the run incremental")
        {
            process_lines(interactive, "(X ~ man) => (X ~ mortal)\njohn ~ man\n.run\n.run-once\nbob ~ man");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "bob" "bob" "~" "mortal"))js");
            CHECK(any_output_contains(collector, "bob=yes"));
        } });
}

// A run takes the record of additions over at its start and declares the
// graph a fixpoint only at its end. `.run-export` to a path it fails to
// access halts the run between the two, and the record stayed both valid and
// empty: the subsequent `.run-delta` seeded nothing and missed
// `plato ~ mortal`, which check mode reports as a completeness violation. A
// run that fails to reach its end now leaves the record invalid, and the next
// incremental request is a full pass. The directory is removed first, so
// that the path cannot be opened.
TEST_CASE("run-delta: falls back to a full pass after a run that an error stopped")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path missing = std::filesystem::temp_directory_path() / "zelph_no_such_directory";
        std::filesystem::remove_all(missing);

        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\nsocrates ~ man\n.run\nplato ~ man");
        interactive.process(kReport);
        CHECK_THROWS_WITH(interactive.process(".run-export " + (missing / "derivations.jsonl").string()),
                          doctest::Contains("Cannot open export file"));

        collector.clear();
        interactive.process(".run-delta");
        CHECK(any_event_contains(collector, "Incremental run not applicable"));
        interactive.process(R"js(%(report "plato" "plato" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "plato=yes")); });
}

TEST_CASE("zelph/run-delta: :quiet says nothing about falling back to a full pass")
{
    // run-delta reverts to a full pass before any run has happened, and a
    // loud run communicates this on the diagnostic channel. A quiet run
    // does not; the fallback behaviour remains unchanged.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(kReport);
        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        collector.clear();
        interactive.process(R"js(%(do (zelph/run-delta :quiet) nil))js");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        CHECK(collector.events().empty());

        interactive.process(R"js(%(report "socrates" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });
}

// This case was "run-delta: falls back to a full pass when a rule was added"
// and pinned the note. The new rule must observe a fact that predates it, and
// seeding from the delta would provide only the rule's own setup, so the whole
// classic pass was kept, including for every rule that had previously
// encountered the older facts. A seeded run now gives the new rule a classic
// pass of its own and seeds the older rules from the record. While a rule in
// force has a negated condition, the whole pass is still executed, since a new
// rule may alter the negation levels.
TEST_CASE("run-delta: a rule added since the last run sees the older facts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);

        const auto add_rule = [&]
        {
            interactive.process(R"js(%(zelph/run))js");
            collector.clear();
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "fallible")))js");
            interactive.process(R"js(%(zelph/run-delta))js");
        };

        SUBCASE("the run stays incremental")
        {
            add_rule();
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
            CHECK(any_output_contains(collector, "socrates=yes"));
        }
        SUBCASE("with a negating rule present it is a full pass")
        {
            interactive.process("((X ~ human), ¬(X ~ god)) => (X ~ earthly)");
            add_rule();
            // The notice goes to the diagnostic channel, not to Out.
            CHECK(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
            CHECK(any_output_contains(collector, "socrates=yes"));
        }
        // A rule in force with a negated condition causes the run to become a
        // full pass upon addition of a rule. A statement that merely mentions
        // such a rule creates none, and the run stays incremental.
        SUBCASE("a negating rule that is only mentioned keeps it incremental")
        {
            interactive.process("john believes (((X ~ human), ¬(X ~ god)) => (X ~ earthly))");
            add_rule();
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
            CHECK(any_output_contains(collector, "socrates=yes"));
        }
        // A new rule that writes a rule: during the first iteration, the
        // classic pass of the new rule produces the inner rule, causing the
        // rule set to expand throughout the seeded run, and the inner rule
        // takes the classic pass that every rule receives whenever the rule set
        // grows. It also meets the older `socrates ~ human`.
        SUBCASE("a rule the new rule writes sees the older facts as well")
        {
            process_lines(interactive, "go ~ trigger\n.run\n(T ~ trigger) => ((Y ~ human) => (Y ~ counted))");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "socrates" "socrates" "~" "counted"))js");
            CHECK(any_output_contains(collector, "socrates=yes"));
        }
        // A removed rule caused the full pass through the rule count, which is
        // no longer decisive by itself. Eliminating a rule derives nothing, yet
        // that rule might have been the sole mention of another one, leaving the
        // latter in force despite having encountered nothing, and the record is
        // unable to reveal this.
        SUBCASE("a rule removed since makes it a full pass")
        {
            process_lines(interactive, ".cluster w\n(X ~ dog) => (X ~ pet)\n.cluster default");
            interactive.process(R"js(%(zelph/run))js");
            interactive.process(".cluster-drop w");
            collector.clear();
            interactive.process(R"js(%(zelph/run-delta))js");
            CHECK(any_event_contains(collector, "Incremental run not applicable"));
        }
        // In the full pass, the older rule matched its 50 instances again.
        // `.semi-naive on` for the count, as the verification pass in check
        // mode re-matches every rule.
        SUBCASE("the older rules are not matched again")
        {
            interactive.process(".semi-naive on");
            interactive.process(R"js(%(for i 0 50 (zelph/fact (string "i" i) "P31" "bigclass")))js");
            process_lines(interactive, "(X P31 bigclass) => (X is mortal)\n.run\nsocrates P31 smallclass\n(X P31 smallclass) => (X is fallible)");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            CHECK(any_event_contains(collector, "Reasoning summary: 1 matches processed,"));
            interactive.process(R"js(%(report "socrates" "socrates" "is" "fallible"))js");
            CHECK(any_output_contains(collector, "socrates=yes"));
        } });
}

// A statement that mentions a rule, `john believes ((A p B) => (A q B))`,
// writes the `=>` fact of a rule that is not currently in force, and that fact
// was incorporated into the record of additions as if it were a new rule: the
// next .run-delta took a full pass for it, which on a substantial graph
// equates to a full sweep across every rule's extent. Only a rule in force is
// considered new within the rule set, even when a rule in force includes a
// negated condition.
TEST_CASE("run-delta: a statement that mentions a rule keeps the run incremental")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\ntim ~ man");
        interactive.process(kReport);

        const auto mention = [&](const std::string& statement)
        {
            interactive.process(".run");
            interactive.process(statement);
            interactive.process("bob ~ man");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "bob" "bob" "~" "mortal"))js");
            CHECK(any_output_contains(collector, "bob=yes"));
        };

        SUBCASE("a rule with variables") { mention("john believes ((A p B) => (A q B))"); }
        SUBCASE("a ground rule") { mention("john believes ((a p b) => (a q b))"); }
        SUBCASE("with a negating rule present")
        {
            interactive.process("((X ~ man), ¬(X ~ god)) => (X ~ earthly)");
            mention("john believes ((A p B) => (A q B))");
            interactive.process(R"js(%(report "bob-earthly" "bob" "~" "earthly"))js");
            CHECK(any_output_contains(collector, "bob-earthly=yes"));
        } });
}

// A ground rule is a single node wherever its text is present, meaning that
// a statement mentioning it,
// `bob believes ((i0 waits nil) => (i0 ~ waiting))`, takes the asserted rule
// itself out of force. When that statement went with its cluster, the rule
// was in force again, yet no indication was given to the record of
// additions: the next .run-delta seeded only what had arrived since the last
// run, and the rule never met `i0 waits nil`, which had been asserted while
// the rule was out of force. In check mode, this constituted a completeness
// violation. A rule that a removal brings into force now counts as a new
// rule and takes the classic pass a new rule takes, or the full pass when a
// rule with a negated condition is in force.
TEST_CASE("run-delta: a rule whose last mention was removed sees the older facts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(i0 waits nil) => (i0 ~ waiting)\n.run");
        interactive.process(kReport);

        const auto drop_mention = [&](const std::string& before_run)
        {
            process_lines(interactive, ".cluster w\nbob believes ((i0 waits nil) => (i0 ~ waiting))\n.cluster default\n" + before_run + ".run\ni0 waits nil\n.run");
            interactive.process(R"js(%(report "mentioned" "i0" "~" "waiting"))js");
            REQUIRE(any_output_contains(collector, "mentioned=no"));

            interactive.process(".cluster-drop w");
            collector.clear();
            interactive.process(".run-delta");
        };

        SUBCASE("the run stays incremental")
        {
            drop_mention("");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
            CHECK(any_output_contains(collector, "i0=yes"));
        }
        // The other mention is entered outside the cluster, yet it refers to
        // `believes`, a node created by the cluster, thus the drop takes it
        // along the general path, the cascade of remove_node, instead of
        // following the fast path taken by the first case.
        SUBCASE("a mention the drop takes along from outside the cluster")
        {
            drop_mention("ann believes ((i0 waits nil) => (i0 ~ waiting))\n");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
            CHECK(any_output_contains(collector, "i0=yes"));

            collector.clear();
            interactive.process("S believes O");
            CHECK(collect_answers(collector).empty());
        }
        // A rule stays out of force when another statement continues to
        // mention it: the record lists it, and the run finds that it is
        // mentioned.
        SUBCASE("a mention that remains keeps the rule out of force")
        {
            drop_mention("ann doubts ((i0 waits nil) => (i0 ~ waiting))\n");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
            CHECK(any_output_contains(collector, "i0=no"));
        }
        SUBCASE("with a negating rule present it is a full pass")
        {
            drop_mention("((X ~ man), ¬(X ~ god)) => (X ~ earthly)\n");
            CHECK(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
            CHECK(any_output_contains(collector, "i0=yes"));
        }
        // Whether the act of removing the last mention constitutes creating
        // the rule depends on whether the rule was mentioned during the most
        // recent run, rather than on whether the last mention was present at
        // that time: the earlier mention from `bob` enters the rule into the
        // record upon its departure, the rule remains out of force while the
        // more recent one from `ann` persists, and after that one also
        // departs, the seeded run treats the rule as new, and it meets
        // `i0 waits nil`, which arrived while the rule was still mentioned.
        // `.help .run-delta` and janet.md express it in this way.
        SUBCASE("a mention made since the run that goes after the older one")
        {
            process_lines(interactive, ".cluster w\nbob believes ((i0 waits nil) => (i0 ~ waiting))\n.cluster default\n"
                                       ".run\ni0 waits nil\n.run");
            interactive.process(R"js(%(report "mentioned" "i0" "~" "waiting"))js");
            REQUIRE(any_output_contains(collector, "mentioned=no"));

            process_lines(interactive, ".cluster v\nann doubts ((i0 waits nil) => (i0 ~ waiting))\n.cluster default\n"
                                       ".cluster-drop w\n.cluster-drop v");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
            CHECK(any_output_contains(collector, "i0=yes"));
        } });
}

// The statement that mentions a rule is not invariably the fact holding its
// `=>`: within an opaque argument, the rule sits in an unclaimed statement
// crafted by the parser, and within a collection, it serves as the subject of
// a membership. The removal hook reads every part of each removed fact that
// existed at the last run, regardless of whether it was claimed, so either
// holder brings the rule into force when its cluster is dropped, and the rule
// meets `i0 waits nil`, which arrived while it was mentioned.
TEST_CASE("run-delta: a rule comes into force when a holder nobody typed goes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto drop_holder = [&](const std::string& setup, const std::string& mention)
        {
            process_lines(interactive, ".auto-run\n" + setup + "(i0 waits nil) => (i0 ~ waiting)\n.run\n.cluster w\n" + mention + "\n.cluster default\n.run\ni0 waits nil\n.run");
            interactive.process(kReport);
            interactive.process(R"js(%(report "mentioned" "i0" "~" "waiting"))js");
            REQUIRE(any_output_contains(collector, "mentioned=no"));

            interactive.process(".cluster-drop w");
            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
            CHECK(any_output_contains(collector, "i0=yes"));
        };

        SUBCASE("the unclaimed statement inside an opaque argument")
        {
            drop_holder("believes ~ opaque\n", "bob believes (carol says ((i0 waits nil) => (i0 ~ waiting)))");
        }
        SUBCASE("the membership of a collection")
        {
            drop_holder("", "bob has {((i0 waits nil) => (i0 ~ waiting)) x}");
        } });
}

// A rule that had been out of force during the last run is new to the next
// seeded run once its last mention goes. A mention made after the last run,
// however, never kept the rule out of that run: the rule has seen every older
// fact, and the record holds what has arrived since. Dropping such a scratch
// mention therefore leaves the run incremental, even when a negating rule is
// in force, where a new rule turns it into a full pass.
TEST_CASE("run-delta: a mention made and dropped since the last run keeps the run incremental")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\n((X ~ man), ¬(X ~ god)) => (X ~ earthly)\n"
                                   "(a p b) => (a q b)\ntim ~ man\n.run\n"
                                   ".cluster dry\njohn believes ((a p b) => (a q b))\n.cluster default\n.cluster-drop dry\nbob ~ man");
        interactive.process(kReport);
        collector.clear();
        interactive.process(".run-delta");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        interactive.process(R"js(%(report "mortal" "bob" "~" "mortal"))js");
        interactive.process(R"js(%(report "earthly" "bob" "~" "earthly"))js");
        CHECK(any_output_contains(collector, "mortal=yes"));
        CHECK(any_output_contains(collector, "earthly=yes")); });
}

// The removal hook enters into the record of additions the `=>` of a rule
// that a removal brought into force, and any fact in the record is considered
// newly created since the most recent run. When that rule was removed
// afterwards, its removal was interpreted as the removal of a new rule, and
// nothing told the next run that a rule had been removed since. In this case,
// the rule removed is a rule generator whose consequence mentions the waiting
// rule, so its removal brings that rule into force, and the seeded run never
// showed that rule `i0 waits nil`: a completeness violation in check mode.
// The rule `(c d e) => (f g h)`, created afterwards, keeps the total number
// of rules at the level it was during the last run; without it, the smaller
// count alone would have made the run a full pass.
TEST_CASE("run-delta: a rule a removal brought into force is a removed rule when it goes itself")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(i0 waits nil) => (i0 ~ waiting)\n"
                                   ".cluster w2\n(a p b) => ((i0 waits nil) => (i0 ~ waiting))\n"
                                   ".cluster w1\nbob believes ((a p b) => ((i0 waits nil) => (i0 ~ waiting)))\n"
                                   ".cluster default\n.run\ni0 waits nil\n.run");
        interactive.process(kReport);
        interactive.process(R"js(%(report "mentioned" "i0" "~" "waiting"))js");
        REQUIRE(any_output_contains(collector, "mentioned=no"));

        process_lines(interactive, ".cluster-drop w1\n.cluster-drop w2\n(c d e) => (f g h)");
        collector.clear();
        interactive.process(".run-delta");
        CHECK(any_event_contains(collector, "Incremental run not applicable"));
        interactive.process(R"js(%(report "i0" "i0" "~" "waiting"))js");
        CHECK(any_output_contains(collector, "i0=yes")); });
}

// The record of additions claims a fixpoint, and a dropped cluster where a run
// derived facts has consequences whose premises reside outside it: `john ~
// mortal` was derived in `w` from `john ~ man`, asserted before the cluster,
// and the next .run-delta found nothing to seed it from until a full run. A
// rule featuring a negated condition derives such a fact again via its classic
// pass, which is why the rule here is positive. The reverse direction required
// a full pass: the drop of a rule constructed within a cluster, or the scratch
// of a rule typed twice, left the removed `=>` fact in the record as a new
// rule.
TEST_CASE("run-delta: falls back to a full pass after a dropped cluster held derivations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\ntim ~ man\n.run-delta\njohn ~ man");
        interactive.process(kReport);

        const auto run_delta = [&]
        {
            collector.clear();
            interactive.process(".run-delta");
            interactive.process(R"js(%(report "john" "john" "~" "mortal"))js");
            interactive.process(R"js(%(report "bill" "bill" "~" "mortal"))js");
            CHECK(any_output_contains(collector, "john=yes"));
            CHECK(any_output_contains(collector, "bill=no"));
        };

        SUBCASE("a run derived into the dropped cluster")
        {
            process_lines(interactive, ".cluster w\nbill ~ man\n.run-delta\n.cluster default\n.cluster-drop w");
            run_delta();
            CHECK(any_event_contains(collector, "Incremental run not applicable (a removal took facts a run may have derived)"));
        }
        SUBCASE("a write-only drop keeps the run incremental")
        {
            process_lines(interactive, ".cluster w\nbill ~ man\n.cluster default\n.cluster-drop w");
            run_delta();
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        }
        // A consequence that nests a statement is written in the scratch
        // originating from the firing, which hands the flag to the cluster
        // along with everything it has recorded.
        SUBCASE("a run that derived into the dropped cluster through a firing's scratch")
        {
            process_lines(interactive, "(X ~ thing) => (k notes (X ~ held))\n.run-delta\n.cluster w\nbill ~ thing\n.run-delta\n.cluster default\n.cluster-drop w");
            run_delta();
            CHECK(any_event_contains(collector, "Incremental run not applicable (a removal took facts a run may have derived)"));
        }
        SUBCASE("a merge into another cluster carries the flag")
        {
            process_lines(interactive, ".cluster w\nbill ~ man\n.run-delta\n.cluster-merge w w2\n.cluster-drop w2");
            run_delta();
            CHECK(any_event_contains(collector, "Incremental run not applicable (a removal took facts a run may have derived)"));
        }
        // A run that claims a rule's ground pattern creates no node, and the
        // drop marks the pattern again: the claim functioned as a derivation
        // too.
        SUBCASE("a claim a run made in the dropped cluster carries the flag")
        {
            process_lines(interactive, "(X ~ king) => (crown is worn)\n.run-delta\nann ~ king\n.cluster w\n.run-delta\n.cluster default\n.cluster-drop w");
            run_delta();
            CHECK(any_event_contains(collector, "Incremental run not applicable (a removal took facts a run may have derived)"));
            interactive.process(R"js(%(report "crown" "crown" "is" "worn"))js");
            CHECK(any_output_contains(collector, "crown=yes"));
        }
        SUBCASE("a rule built in a write-only dropped cluster does not force a full pass")
        {
            process_lines(interactive, ".cluster w\n(X ~ mortal) => (X ~ living)\n.cluster default\n.cluster-drop w");
            run_delta();
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        }
        SUBCASE("a rule re-entered through dedup-rule does not force a full pass")
        {
            interactive.process("(X ~ man) => (X ~ mortal)");
            run_delta();
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        } });
}

// The same class lacking a cluster: a pruned derived fact remained absent
// following the next .run-delta, which was seeded exclusively from what was
// newly introduced. A removal command is unable to distinguish a derived fact
// from an asserted one, thus every fact-removal operation results in the
// record being surrendered, and the next .run-delta performs a full pass.
TEST_CASE("run-delta: falls back to a full pass after a pruned derived fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\njohn ~ man\ntim ~ man\n.run");
        interactive.process(kReport);

        const auto run_delta = [&]
        {
            interactive.process("ann ~ cat");
            collector.clear();
            interactive.process(".run-delta");
            CHECK(any_event_contains(collector, "Incremental run not applicable (a removal took facts a run may have derived)"));
            interactive.process(R"js(%(report "john" "john" "~" "mortal"))js");
        };

        SUBCASE(".prune-facts of the fact")
        {
            interactive.process(".prune-facts john ~ mortal");
            run_delta();
            CHECK(any_output_contains(collector, "john=yes"));
        }
        SUBCASE(".prune-facts by a pattern")
        {
            interactive.process(".prune-facts X ~ mortal");
            run_delta();
            CHECK(any_output_contains(collector, "john=yes"));
        }
        SUBCASE("zelph/prune-fact of the fact")
        {
            interactive.process(R"js(%(zelph/prune-fact "john" "~" "mortal"))js");
            run_delta();
            CHECK(any_output_contains(collector, "john=yes"));
        }
        SUBCASE("zelph/prune-fact of the fact where a rule also names it")
        {
            // A withdrawn claim leaves the node as the rule's pattern, but the
            // fact that a run derived is no longer considered data, and a run
            // seeded exclusively from what is new would not derive it again:
            // a withdrawal gives the record up in the same manner as a
            // removal does.
            process_lines(interactive, "(john ~ mortal) => (john is gone)\n.run\n");
            interactive.process(R"js(%(zelph/out (string (zelph/prune-fact "john" "~" "mortal"))))js");
            REQUIRE(last_out_text(collector) == "unclaimed");
            run_delta();
            CHECK(any_output_contains(collector, "john=yes"));
        }
        SUBCASE(".prune-nodes")
        {
            interactive.process(".prune-nodes T ~ man");
            run_delta();
        }
        SUBCASE(".remove")
        {
            interactive.process(".remove tim");
            run_delta();
            CHECK(any_output_contains(collector, "john=yes"));
        } });
}

// What a removal does to the record when it keeps it: a fact previously listed
// in the record disappears from it, and a fact that held at the last run
// leaves its predicate, which a seeded run consults to determine whether a
// negation could have turned true. The removal of rule text that the engine
// writes leaves nothing: `~` serves as the predicate for every class fact, and
// a pattern or marking associated with a rule would otherwise be interpreted
// as a lost class fact.
TEST_CASE("run-delta: a removal keeps the record of what was added since the last run true")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        const auto removed = [&]
        {
            auto v = graph->removed_predicates_since_run();
            std::sort(v.begin(), v.end());
            return v;
        };
        const auto sorted = [](std::vector<zelph::network::Node> v)
        {
            std::sort(v.begin(), v.end());
            return v;
        };

        // `likes` is declared outside the cluster, ensuring its declaration
        // does not fall within what the cluster holds.
        process_lines(interactive, ".auto-run\n(X ~ man) => (X ~ mortal)\n(a p b) => (c q d)\nx likes y\n.cluster w\nbob likes cake\n.cluster default\ntim ~ man");

        SUBCASE("what a dropped cluster created since the run leaves no entry")
        {
            process_lines(interactive, ".run\n.cluster w2\nann likes pie\n.cluster default");
            CHECK(graph->delta_entries() > 0);
            interactive.process(".cluster-drop w2");
            CHECK(graph->delta_entries() == 0);
            CHECK(removed().empty());
        }
        SUBCASE("a fact that held at the run leaves its predicate")
        {
            process_lines(interactive, ".run\n.cluster-drop w");
            CHECK(graph->delta_entries() == 0);
            CHECK(removed() == std::vector<zelph::network::Node>{graph->get_node("likes")});
        }
        SUBCASE("a drop that runs the cascade leaves the predicates of what it took")
        {
            process_lines(interactive, "(bob likes cake) seen-by k\n.run\n.cluster-drop w");
            CHECK(removed() == sorted({graph->get_node("likes"), graph->get_node("seen-by")}));
        }
        SUBCASE("a claimed rule pattern leaves nothing")
        {
            process_lines(interactive, ".run\na p b");
            CHECK(removed().empty());
        }
        SUBCASE("a removed rule leaves =>")
        {
            process_lines(interactive, ".run\n.remove-rules");
            CHECK(removed() == std::vector<zelph::network::Node>{graph->core.Causes});
        }
        SUBCASE("a rule dropped with its cluster leaves =>, and its patterns nothing")
        {
            process_lines(interactive, ".cluster w3\n(X ~ dog) => (X ~ pet)\n.cluster default\n.run\n.cluster-drop w3");
            CHECK(removed() == std::vector<zelph::network::Node>{graph->core.Causes});
        }
        SUBCASE("the scratch of a rule typed twice leaves nothing")
        {
            process_lines(interactive, ".run\n(X ~ man) => (X ~ mortal)");
            CHECK(graph->delta_entries() == 0);
            CHECK(removed().empty());
        }
        SUBCASE("a run clears it")
        {
            process_lines(interactive, ".run\n.cluster-drop w\n.run");
            CHECK(removed().empty());
        } });
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

TEST_CASE("run-delta: a rule an import brings sees the older facts")
{
    // This case was "run-delta: falls back when an import brings a rule" and
    // pinned the note. A rule received via import must observe the facts
    // that existed beforehand, which precisely matches what the skipped
    // classic pass would have revealed; the seeded run now gives that pass
    // exclusively to the new rule, just as it does for a rule typed since
    // the last run (see "run-delta: a rule added since the last run sees the
    // older facts"), and takes the full pass when a rule in force has a
    // negated condition.
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

        const auto import_rule = [&]
        {
            interactive.process(R"js(%(zelph/run))js");
            collector.clear();
            interactive.process(".import \"" + script.string() + "\"");
            interactive.process(R"js(%(zelph/run-delta))js");
        };

        SUBCASE("the run stays incremental")
        {
            import_rule();
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        }
        SUBCASE("with a negating rule present it is a full pass")
        {
            interactive.process("((X ~ human), ¬(X ~ god)) => (X ~ earthly)");
            import_rule();
            CHECK(any_event_contains(collector, "Incremental run not applicable"));
        }

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

TEST_CASE("zelph/auto-run?: reports .auto-run, and false while a module is imported")
{
    // The switch was not read by any binding, meaning a keyword handler
    // performing inference on its own could not tell whether the host would
    // run after its block as well. Within a module, the host does not: the
    // import suspends auto-run and executes just once following the module's
    // last line, causing the binding to return false in that context. A load
    // disables auto-run for the entire session, and the binding reflects
    // this state accordingly.
    namespace fs          = std::filesystem;
    const fs::path module = fs::temp_directory_path() / "zelph_test_auto_run_module.zph";
    const fs::path saved  = fs::temp_directory_path() / "zelph_test_auto_run_load.bin";
    {
        std::ofstream f(module);
        f << R"js(%(zelph/out (string "inside=" (zelph/auto-run?))))js" << "\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const char* expected)
        {
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "AUTO-" (zelph/auto-run?))))js");
            return any_output_contains(collector, expected);
        };

        SUBCASE("the switch")
        {
            CHECK(answers("AUTO-true"));
            interactive.process(".auto-run");
            CHECK(answers("AUTO-false"));
            interactive.process(".auto-run");
            CHECK(answers("AUTO-true"));
        }
        SUBCASE("a module import")
        {
            collector.clear();
            interactive.process(".import \"" + module.generic_string() + "\"");
            CHECK(any_output_contains(collector, "inside=false"));
            CHECK(answers("AUTO-true"));
        }
        SUBCASE("a load switches it off")
        {
            interactive.process("a p b");
            interactive.process(".save \"" + saved.generic_string() + "\"");
            interactive.process(".load \"" + saved.generic_string() + "\"");
            CHECK(answers("AUTO-false"));
        } });

    std::error_code ec;
    fs::remove(module, ec);
    fs::remove(saved, ec);
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

        // A condition set does not respond to any query, thus the read
        // surface tracks their counts.
        collector.clear();
        interactive.process(R"(%(zelph/out (string "SETS=" (length (zelph/sources "~" "conjunction")))))");
        CHECK(any_output_contains(collector, "SETS=0"));

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
        // A condition set does not respond to any query, thus the read
        // surface tracks their counts.
        collector.clear();
        interactive.process(R"(%(zelph/out (string "SETS=" (length (zelph/sources "~" "conjunction")))))");
        CHECK(any_output_contains(collector, "SETS=0"));

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
        // A condition set does not respond to any query, thus the read
        // surface tracks their counts.
        collector.clear();
        interactive.process(R"(%(zelph/out (string "SETS=" (length (zelph/sources "~" "conjunction")))))");
        CHECK(any_output_contains(collector, "SETS=0"));

        interactive.process("a p b");
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

// A predicate that no condition binds is rejected upon the writing of a
// typed rule ("fresh variables: a predicate that no condition binds is
// refused when the rule is written"), and the Janet builders generate
// identical rules: zelph/dedup-rule serves as the route of a program that
// builds rules, `ace` being one of them. Each kept the rule, which either
// derived nothing or made a witness functioning as a predicate. zelph/rule*
// evaluates its arguments prior to execution, so they stay as the claims
// they are; the remaining builders keep nothing.
TEST_CASE("zelph/rule: every builder refuses a consequence whose predicate no condition binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string call;
        std::string builder;
        SUBCASE("zelph/dedup-rule")
        {
            call    = R"js(%(zelph/dedup-rule (fn [] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")))))js";
            builder = "";
        }
        SUBCASE("the macro")
        {
            call    = R"js(%(zelph/rule [(zelph/fact 'X "waits" "nil")] (zelph/fact 'X 'W "nil")))js";
            builder = "zelph/rule: ";
        }
        SUBCASE("the plain function")
        {
            interactive.process(R"js(%(def [condition consequence] [(zelph/fact 'X "waits" "nil") (zelph/fact 'X 'W "nil")]))js");
            call    = "%(zelph/rule* [condition] consequence)";
            builder = "zelph/rule: ";
        }
        SUBCASE("zelph/build-rule")
        {
            call    = R"js(%(zelph/build-rule (fn [] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")))))js";
            builder = "zelph/build-rule: ";
        }
        // A thunk can write the consequences as several `=>` facts under a
        // single condition and return one of them (build_marked_rule), with
        // each fact constituting a rule currently in force: only the returned
        // one underwent verification, while the other remained, a rule that
        // derived nothing.
        SUBCASE("zelph/build-rule: the `=>` fact the thunk does not return")
        {
            call = R"js(%(zelph/build-rule (fn [] (def c (zelph/fact 'X "waits" "nil")) (zelph/fact c "=>" (zelph/fact 'X 'W "nil")) (zelph/fact c "=>" (zelph/fact 'X "q" "nil")))))js";
            builder = "zelph/build-rule: ";
        }

        const std::size_t before = node_count(collector, interactive);
        CHECK_THROWS_WITH(interactive.process(call), doctest::Contains(builder + "The predicate of \"X W nil\" in the rule's consequence is the variable W"));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// The `=>` fact a builder produces may also be expressed using zelph/fact, and
// nothing verified it there: given
// `%(zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil"))`,
// .list-rules listed the rule, and via `ann waits nil` the query `S P nil`
// answered `-> -> nil`. The call cannot reject it, since an inner rule of a
// generator maintains identical structure until the generator holds it (refer
// to the next case), thus the rule undergoes validation when the block ends,
// and is removed if it is still in force.
TEST_CASE("zelph/fact: a `=>` whose consequence predicate no condition binds is refused at the end of its block")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string call;
        std::string written;
        SUBCASE("the predicate alone")
        {
            call    = R"js(%(zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")))js";
            written = "X W nil";
        }
        SUBCASE("the predicate and the subject")
        {
            call    = R"js(%(zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")))js";
            written = "W W nil";
        }
        SUBCASE("a condition set")
        {
            call    = R"js(%(zelph/fact (zelph/conjunction (zelph/fact 'X "waits" "nil") (zelph/fact 'X "is" "here")) "=>" (zelph/fact 'W 'W "nil")))js";
            written = "W W nil";
        }
        SUBCASE("a fact nested in the consequence")
        {
            call    = R"js(%(zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X "says" (zelph/fact 'W 'W "nil"))))js";
            written = "W W nil";
        }

        CHECK_THROWS_WITH(interactive.process(call), doctest::Contains("zelph/fact: The predicate of \"" + written + "\" in the rule's consequence is the variable W"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        interactive.process("ann waits nil");
        interactive.process("ann is here");
        collector.clear();
        interactive.process("S P nil");
        interactive.process("ann says O");
        CHECK_FALSE(any_output_contains(collector, "-> ->")); });
}

// The inner rule of a generator takes its predicate from the generator's
// binding, meaning that while zelph/fact writes it, it maintains the shape of
// the rule previously refused. Upon reaching the end of the block, the
// generator holds it, and it passes -- this is the method Janet uses to
// construct a rule generator.
TEST_CASE("zelph/fact: a raw rule generator whose inner predicate the generator binds is kept")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/fact (zelph/fact 'R "inverse" 'S) "=>" (zelph/fact (zelph/fact 'A 'R 'B) "=>" (zelph/fact 'B 'S 'A))))js");
        interactive.process("parent inverse child");
        interactive.process("ann parent bob");
        collector.clear();
        interactive.process("S child O");
        CHECK(any_output_contains(collector, "Answer: bob child ann")); });
}

// A rule that a statement holds is rule text, not a rule in force, and the
// check leaves it untouched, just as it leaves a typed one.
TEST_CASE("zelph/fact: a raw rule a statement of the same block holds is kept, and not in force")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        CHECK_NOTHROW(interactive.process(R"js(%(zelph/fact "draft" "contains" (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil"))))js"));
        CHECK(any_output_contains(collector, "draft contains ((X waits nil) => (X W nil))"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// zelph/rule* evaluates its arguments prior to execution, meaning a nested
// rule handed to it is written by zelph/fact beyond the scope of any build,
// and the rule generated by the call holds it before the block ends.
TEST_CASE("zelph/rule*: an inner rule built raw before the call is kept, and the generator fires")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/rule* [(zelph/fact 'R "inverse" 'S)] (zelph/fact (zelph/fact 'A 'R 'B) "=>" (zelph/fact 'B 'S 'A))))js");
        interactive.process("parent inverse child");
        interactive.process("ann parent bob");
        collector.clear();
        interactive.process("S child O");
        CHECK(any_output_contains(collector, "Answer: bob child ann")); });
}

// The expense incurred when verifying at the end of a block: a raw inner rule
// that only a later block would hold is a rule in force at the conclusion of
// its own block, and is rejected at that point. The generator is formulated
// within a single block, or within a single zelph/build-rule or
// zelph/rule-text thunk.
TEST_CASE("zelph/fact: a raw `=>` that only a later block would hold is refused at the end of its own block")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH(interactive.process(R"js(%(def inner (zelph/fact (zelph/fact 'A "parent" 'B) "=>" (zelph/fact 'B 'S 'A))))js"),
                          doctest::Contains("zelph/fact: The predicate of \"B S A\" in the rule's consequence is the variable S"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// A run within the block would fire the rule before the block's termination,
// hence the run performs a preliminary check; a refusal constitutes the run's
// error, and the run does not take place. By that point, the rule has been
// removed, so a program that catches the error proceeds onward without it.
TEST_CASE("zelph/fact: a run inside the block checks the raw `=>` facts first")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run"); // deactivated, so that just the block's own run fires
        interactive.process("ann waits nil");

        std::string run;
        std::string name;
        SUBCASE("zelph/run")
        {
            run  = "(zelph/run)";
            name = "zelph/run";
        }
        SUBCASE("zelph/run-once")
        {
            run  = "(zelph/run-once)";
            name = "zelph/run-once";
        }
        SUBCASE("zelph/run-delta, quiet")
        {
            run  = "(zelph/run-delta :quiet)";
            name = "zelph/run-delta";
        }

        const std::string rule = R"js((zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")))js";
        CHECK_THROWS_WITH(interactive.process("%(do " + rule + " " + run + ")"),
                          doctest::Contains(name + ": zelph/fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        collector.clear();
        interactive.process(R"js(%(do )js" + rule + R"js( (try )js" + run + R"js( ([e] (zelph/out "caught"))) (zelph/out (string "rules=" (length (zelph/rules))))))js");
        CHECK(any_output_contains(collector, "caught"));
        CHECK(any_output_contains(collector, "rules=0"));

        interactive.process(".run");
        collector.clear();
        interactive.process("S P nil");
        CHECK_FALSE(any_output_contains(collector, "-> ->")); });
}

// A save within the block wrote the rule into the file before the block's
// end could check it, enabling a subsequent load to retrieve it as a rule
// already present in the graph, which no check removes (the case "naming a
// `=>` the graph already holds leaves it in place"). The save performs its
// verification first, just as a run does: a refusal constitutes the error of
// the save, and the file remains unwritten. A `.save` line in a file
// imported by the block functions identically to that save.
TEST_CASE("zelph/fact: a save inside the block checks the raw `=>` facts first")
{
    const std::string path   = (std::filesystem::temp_directory_path() / "zelph_test_fact_rule_save.bin").generic_string();
    const std::string module = (std::filesystem::temp_directory_path() / "zelph_test_fact_rule_save.zph").generic_string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        std::filesystem::remove(path);
        std::string save;
        std::string name;
        SUBCASE("zelph/save")
        {
            save = R"js((zelph/save ")js" + path + R"js("))js";
            name = "zelph/save: ";
        }
        SUBCASE("a .save line of a module the block imports")
        {
            std::ofstream(module) << ".save " << path << "\n";
            save = R"js((zelph/import ")js" + module + R"js("))js";
            name = ".save " + path + "\": ";
        }
        // A slice holds the rules governing its predicates, and it held the raw
        // rule without verification: after being loaded, the rule was already
        // present in the graph, and no subsequent check reverses this.
        SUBCASE("a .save-predicates line of a module the block imports")
        {
            std::ofstream(module) << ".save-predicates " << path << " waits\n";
            save = R"js((zelph/import ")js" + module + R"js("))js";
            name = ".save-predicates " + path + " waits\": ";
        }

        const std::string rule = R"js((zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")))js";
        CHECK_THROWS_WITH(interactive.process("%(do " + rule + " " + save + ")"),
                          doctest::Contains(name + "zelph/fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));
        CHECK_FALSE(std::filesystem::exists(path));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        interactive.process(".save " + path);
        interactive.process(".load " + path);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
    std::filesystem::remove(path);
    std::filesystem::remove(module);
}

// After a Janet error, the graph retains the output that the block wrote
// before the error, a rule in force among it, so the block is still checked,
// and its error carries both messages.
TEST_CASE("zelph/fact: a block that fails still has its raw `=>` facts checked, and reports both errors")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        try
        {
            interactive.process(R"js(%(do (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")) (error "boom")))js");
            FAIL("the block did not fail");
        }
        catch (const std::exception& ex)
        {
            const std::string message = ex.what();
            CHECK(message.find("boom") != std::string::npos);
            CHECK(message.find("zelph/fact: The predicate of \"W W nil\"") != std::string::npos);
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// The boundary: a predicate that a condition binds is written on each firing,
// and the rule remains unchanged from its prior state before the check.
TEST_CASE("zelph/fact: a raw `=>` whose consequence predicate a condition binds is kept and fires")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/fact (zelph/fact 'X 'W "nil") "=>" (zelph/fact 'X 'W "ok")))js");
        interactive.process("ann waits nil");
        collector.clear();
        interactive.process("ann waits O");
        CHECK(any_output_contains(collector, "Answer: ann waits ok")); });
}

TEST_CASE("zelph/fact: two refused `=>` facts in one block are both removed and both named")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        try
        {
            interactive.process(R"js(%(do (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")) (zelph/fact (zelph/fact 'X "sleeps" "nil") "=>" (zelph/fact 'X 'V "nil"))))js");
            FAIL("the block was not refused");
        }
        catch (const std::exception& ex)
        {
            const std::string message = ex.what();
            CHECK(message.find("zelph/fact: The predicate of \"X W nil\"") != std::string::npos);
            CHECK(message.find("zelph/fact: The predicate of \"X V nil\"") != std::string::npos);
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// Removing the refused rule through node-by-node removal would disarm the
// genuine-structure store for the rest of the session (Zelph::drop_cluster),
// causing every subsequent query to traverse the adjacency instead.
TEST_CASE("zelph/fact: the refusal of a raw `=>` keeps the fact stores armed")
{
    run_both_modes([](auto&, auto& interactive)
                   {
        REQUIRE(interactive.graph()->fact_stores_enabled());
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")))js"),
                          doctest::Contains("zelph/fact: The predicate of \"X W nil\""));
        CHECK(interactive.graph()->fact_stores_enabled()); });
}

// The check reads the graph exactly as the block leaves it: a `=>` fact that a
// cluster drop within the same block took back no longer exists to refuse.
TEST_CASE("zelph/fact: a raw `=>` that a cluster drop took back is not reported")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_NOTHROW(interactive.process(R"js(%(do (zelph/cluster "trial") (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")) (zelph/cluster nil) (zelph/cluster-drop "trial")))js"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// The rule that a refused one held as a consequence constitutes part of
// nothing after the refused rule has vanished, and is in force from that
// moment onward. It is verified thereafter, even though the holder had kept it
// out of the initial inspection.
TEST_CASE("zelph/fact: an inner rule that only a refused rule held is checked once that rule is gone")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        try
        {
            interactive.process(R"js(%(let [inner (zelph/fact (zelph/fact 'A "q" 'B) "=>" (zelph/fact 'B 'S 'A))] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X 'W 'Y) inner)))js");
            FAIL("the block was not refused");
        }
        catch (const std::exception& ex)
        {
            const std::string message = ex.what();
            CHECK(message.find("zelph/fact: The predicate of \"X W Y\"") != std::string::npos);
            CHECK(message.find("zelph/fact: The predicate of \"B S A\"") != std::string::npos);
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// The check takes back only a `=>` fact that the block generated or claimed
// into force (next case). Naming a rule the graph already contains creates
// nothing, so a rule already in force -- whether its holder has gone since, as
// in this case, or whether it is a rule a file holds that was saved prior to
// the check's existence -- is not removed by a block that names it again.
TEST_CASE("zelph/fact: naming a `=>` the graph already holds leaves it in place")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(def parts [(zelph/fact 'X "waits" "nil") (zelph/fact 'X 'W "nil")]))js");
        interactive.process(R"js(%(let [rule (zelph/fact (parts 0) "=>" (parts 1))] (zelph/cluster "draft") (zelph/fact "draft" "contains" rule) (zelph/cluster nil)))js");
        interactive.process(R"js(%(zelph/cluster-drop "draft"))js");

        CHECK_NOTHROW(interactive.process(R"js(%(zelph/fact (parts 0) "=>" (parts 1)))js"));
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X waits nil) => (X W nil)")); });
}

// A rule authored by zelph/mention is out of force, yet a claim with
// zelph/fact puts it in force, just as the claim of any mention does. Without
// verification, that claim put a rule into force whose consequence predicate
// is not bound by any condition: following `ann waits nil`, `S P nil`
// responded with `-> -> nil`. The claim is validated akin to a rule that the
// block generates, though the rule predated the call, and the call did nothing
// to it other than claim it: the refusal takes the claim back, and the rule
// stays a mention. Writing it once again as a mention finds it in its proper
// place, and attempting to claim it again is rejected once more, which a rule
// left in force would not be (the case described earlier).
TEST_CASE("zelph/fact: a mentioned `=>` that a block claims into force is checked, and the refusal takes back the claim alone")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(def parts [(zelph/fact 'X "waits" "nil") (zelph/fact 'W 'W "nil")]))js");
        const std::string mention = R"js(%(zelph/mention (parts 0) "=>" (parts 1)))js";
        const std::string claim   = R"js(%(zelph/fact (parts 0) "=>" (parts 1)))js";
        std::string       block   = claim;
        SUBCASE("mentioned in an earlier block")
        {
            interactive.process(mention);
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(any_output_contains(collector, "No rules found"));
        }
        SUBCASE("mentioned in the same block")
        {
            block = R"js(%(do (zelph/mention (parts 0) "=>" (parts 1)) (zelph/fact (parts 0) "=>" (parts 1))))js";
        }

        CHECK_THROWS_WITH(interactive.process(block), doctest::Contains("zelph/fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        const std::size_t before = node_count(collector, interactive);
        interactive.process(mention);
        CHECK(node_count(collector, interactive) == before);

        interactive.process("ann waits nil");
        collector.clear();
        interactive.process("S P nil");
        CHECK_FALSE(any_output_contains(collector, "-> ->"));

        CHECK_THROWS_WITH(interactive.process(claim), doctest::Contains("zelph/fact: The predicate of \"W W nil\"")); });
}

// The value of a thunk is a rule exclusively for the builder it is handed to.
// A bare `=>` fact that the thunk of zelph/rule-text returns at the top of a
// block constitutes a rule in force, just as a second `=>` fact does that the
// thunk of zelph/build-rule writes over a condition other than that of the
// rule it returns: the builder examines solely that rule (build_marked_rule).
// Each was in force without verification, and `S P nil` answered `-> -> nil`.
TEST_CASE("zelph/fact: a raw `=>` written inside a rule build is checked when it ends up in force")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string call;
        SUBCASE("the bare `=>` a zelph/rule-text thunk returns")
        {
            call = R"js(%(zelph/rule-text (fn [] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")))))js";
        }
        SUBCASE("a second `=>` in a zelph/build-rule thunk, over another condition")
        {
            call = R"js(%(zelph/build-rule (fn [] (zelph/fact (zelph/fact 'Y "waits" "nil") "=>" (zelph/fact 'W 'W "nil")) (zelph/fact (zelph/fact 'X "sleeps" "nil") "=>" (zelph/fact 'X "is" "ok")))))js";
        }

        CHECK_THROWS_WITH(interactive.process(call), doctest::Contains("zelph/fact: The predicate of \"W W nil\" in the rule's consequence is the variable W"));

        interactive.process("ann waits nil");
        collector.clear();
        interactive.process("S P nil");
        CHECK_FALSE(any_output_contains(collector, "-> ->")); });
}

// The mention side of the case above. A `=>` fact that zelph/mention
// writes as its statement, either via a thunk or in the form that names the
// subject, the predicate, and the object, constitutes a mention: it is not
// in force, thus the check at the end of the block leaves it untouched,
// `.list-rules` excludes it from its output, and it fires on nothing. A
// zelph/rule-text thunk that returns the identical bare `=>` writes a rule
// that is in force, which the check refuses.
TEST_CASE("zelph/mention: a bare `=>` whose consequence predicate no condition binds is a mention, kept and not in force")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string call;
        SUBCASE("a thunk")
        {
            call = R"js(%(zelph/mention (fn [] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")))))js";
        }
        SUBCASE("the subject, the predicate and the object")
        {
            call = R"js(%(zelph/mention (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'W 'W "nil")))js";
        }

        collector.clear();
        CHECK_NOTHROW(interactive.process(call));
        CHECK(any_output_contains(collector, "(X waits nil) => (W W nil)"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        interactive.process("ann waits nil");
        collector.clear();
        interactive.process("S P nil");
        CHECK_FALSE(any_output_contains(collector, "-> ->")); });
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
            // A thunk can express a rule's two consequences as two `=>`
            // facts sharing a single condition set. It returns one of
            // them, specifically the final form in this context, while
            // the other consequence remains equally part of the rule's
            // text alongside it, and both are marked during the build.
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

TEST_CASE("zelph/lookup, zelph/lookup-all, zelph/degree, zelph/parse and zelph/epoch: a Janet thread is answered")
{
    // The lookups that create nothing merely perform reads, so a
    // thread evaluating a compiled network can invoke them, in contrast
    // to zelph/resolve, where forming a node constitutes a write.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        interactive.process(on_janet_thread(R"js((string/format "%q %q %q %q %q" (zelph/lookup "a") (zelph/lookup "zz") (length (zelph/lookup-all "a")) (zelph/degree "a") (get (zelph/parse (zelph/lookup "a")) 0)))js"));
        CHECK(last_out_text(collector) == "returned: <zelph/node a> nil 1 2 nil");

        collector.clear();
        interactive.process(on_janet_thread(R"js((> (zelph/epoch) 0))js"));
        CHECK(last_out_text(collector) == "returned: true");

        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("zelph/annotations: a Janet thread is answered")
{
    // Annotations are recorded exclusively on the main thread, and
    // reading them induces no modifications, allowing a thread to
    // reference what a front end has documented regarding a
    // node -- just as it can ask zelph/lookup.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process(R"(%(zelph/annotate "a" "source" "a sentence"))");

        collector.clear();
        interactive.process(on_janet_thread(R"js((string/format "%q %q" (zelph/annotations "a" "source") (zelph/annotations "b")))js"));
        CHECK(last_out_text(collector) == R"(returned: ("a sentence") {})"); });
}

TEST_CASE("Janet threads: a name is read in one language while the main thread switches it")
{
    // A thread's reads that take the current language -- zelph/lookup,
    // zelph/resolve, zelph/lookup-all when no language is specified, and any
    // binding that resolves a string without creating it -- read the string the
    // main thread assigns on each `.lang`, without synchronization: a thread
    // querying "dog" while the main thread switched between two languages that
    // both name a dog occasionally found none. Each read now takes the language
    // the main thread most recently published, whole, and one binding resolves
    // all its strings within a single language.
    //
    // The thread performs reading from before the initial switch through to
    // beyond the final one, meaning the switches reside within its range of
    // reads; the outcome remains unaffected by the manner in which the two
    // sequences interleave. The languages are sufficiently lengthy that their
    // names cannot be contained within a string object.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const std::string           la = "first-language-of-this-session-aaaaaaaa";
    const std::string           lb = "the-other-language-of-this-session-bbbbbbbbbbbb";
    interactive.process(".lang " + la);
    interactive.process("dog ~ animal");
    interactive.process("hund barks loud");
    interactive.process(".name hund " + lb + " chien");
    interactive.process(".lang " + lb);
    interactive.process("dog ~ animal");
    interactive.process("dog ~ pet");
    interactive.process(".lang zelph");

    // `ok` is a Janet expression that the thread evaluates following each
    // read; da and db represent the degrees associated with the two dogs.
    const auto while_switching = [&](const std::string& ok)
    {
        collector.clear();
        interactive.process(
            R"js(%(do (def la ")js" + la + R"js(") (def lb ")js" + lb
            + R"js(") (def da (zelph/degree (zelph/lookup "dog" la))) (def db (zelph/degree (zelph/lookup "dog" lb))) (zelph/lang la))js"
              R"js( (def started (ev/thread-chan 1)) (def stop (ev/thread-chan 1)) (def result (ev/thread-chan 1)))js"
              R"js( (ev/spawn-thread (ev/give started true) (ev/give result (try (do (var reads 0) (var wrong 0) (var going true))js"
              R"js( (while going (unless )js"
            + ok
            + R"js( (++ wrong)) (++ reads) (when (> (ev/count stop) 0) (set going false))))js"
              R"js( (string "reads " (if (> reads 0) "some" "none") ", wrong " wrong)) ([e] (string "refused: " e))))))js"
              R"js( (ev/take started) (for i 0 20000 (zelph/lang (if (even? i) la lb))))js"
              R"js( (ev/give stop true) (def r (ev/take result)) (zelph/lang "zelph") (zelph/out r)))js");
        return last_out_text(collector);
    };

    SUBCASE("a name looked up by itself")
    {
        CHECK(while_switching(R"js((let [d (zelph/degree (zelph/lookup "dog")) r (zelph/degree (zelph/resolve "dog")) g (zelph/degree "dog")] (and (or (= d da) (= d db)) (or (= r da) (= r db)) (or (= g da) (= g db)) (= 1 (length (zelph/lookup-all "dog"))))))js")
              == "reads some, wrong 0");
    }
    SUBCASE("the parts of one fact")
    {
        CHECK(while_switching(R"js((and (zelph/exists "dog" "~" "animal") (zelph/mentioned "dog" "~" "animal") (> (length (zelph/targets "dog" "~")) 0) (= 1 (length (zelph/sources "~" "animal")))))js")
              == "reads some, wrong 0");
    }
    SUBCASE("a node's name and its printed form")
    {
        // zelph/name without a language and a node's `string` (Zelph::format)
        // read the language too. The node is searched within a fixed language,
        // meaning only the name read might be incorrect: one node, named in
        // both languages.
        CHECK(while_switching(R"js((let [c (zelph/lookup "hund" la) n (zelph/name c :strict) s (string c)] (and (or (= n "hund") (= n "chien")) (or (= s "hund") (= s "chien")))))js")
              == "reads some, wrong 0");
    }

    collector.clear();
    interactive.process(".lang");
    CHECK(any_output_contains(collector, "'zelph'"));
}

TEST_CASE("engine: a name read without a language takes the one the main thread published")
{
    // An empty language argument means the current language, and the
    // engine's internal readers took it from the string that set_lang
    // assigns, without synchronization. No caller on a different thread
    // currently passes an empty language -- the Janet bindings independently
    // read the published language, as demonstrated in the earlier case -- yet
    // one that did would occasionally encounter no dog, just as a Janet thread
    // previously did. A thread distinct from the main thread may read names:
    // Janet threads do so, as do the C API functions zelph_resolve and
    // zelph_name. The three readers that default to the language read the
    // one set_lang has published, so a binding depending on this default
    // cannot bring the race back.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const std::string           la = "first-language-of-this-session-aaaaaaaa";
    const std::string           lb = "the-other-language-of-this-session-bbbbbbbbbbbb";
    interactive.process(".lang " + la);
    interactive.process("dog ~ animal");
    interactive.process("hund barks loud");
    interactive.process(".name hund " + lb + " chien");
    interactive.process(".lang " + lb);
    interactive.process("dog ~ animal");
    interactive.process(".lang zelph");

    zelph::network::Reasoning* const graph = interactive.graph();
    const zelph::network::Node       da    = graph->get_node("dog", la);
    const zelph::network::Node       db    = graph->get_node("dog", lb);
    const zelph::network::Node       hund  = graph->get_node("hund", la);
    REQUIRE(da != 0);
    REQUIRE(db != 0);
    REQUIRE(da != db);
    REQUIRE(hund != 0);

    const auto while_switching = [&](const std::function<bool()>& ok)
    {
        graph->set_lang(la);
        std::atomic<bool> started{false};
        std::atomic<bool> stop{false};
        std::size_t       reads = 0;
        std::size_t       wrong = 0;
        std::thread       reader([&]
                                 {
            started.store(true);
            do
            {
                if (!ok()) ++wrong;
                ++reads;
            } while (!stop.load()); });
        while (!started.load())
            std::this_thread::yield();
        for (int i = 0; i < 20000; ++i)
            graph->set_lang(i % 2 == 0 ? lb : la);
        stop.store(true);
        reader.join();
        graph->set_lang("zelph");
        return std::string(reads > 0 ? "reads some" : "reads none") + ", wrong " + std::to_string(wrong);
    };

    SUBCASE("get_node")
    {
        CHECK(while_switching([&]
                              { const zelph::network::Node n = graph->get_node("dog"); return n == da || n == db; })
              == "reads some, wrong 0");
    }
    SUBCASE("nodes_named")
    {
        CHECK(while_switching([&]
                              { const auto c = graph->nodes_named("dog"); return c && c->size() == 1 && ((*c)[0] == da || (*c)[0] == db); })
              == "reads some, wrong 0");
    }
    SUBCASE("get_name")
    {
        CHECK(while_switching([&]
                              { const std::string n = graph->get_name(hund); return n == "hund" || n == "chien"; })
              == "reads some, wrong 0");
    }
}

TEST_CASE("Janet threads: a thread reads the language the main thread set last")
{
    // janet.md guarantees the language the main thread set last, rather than
    // the one present when the thread was spawned: capturing a snapshot at the
    // moment the thread's execution begins would have similarly ensured the
    // prior read was race-free, causing a thread spawned before a `.lang` to
    // continue reading names in the old language. The thread is spawned under
    // `de` and performs its read following the switch to `fr`, which the
    // channel orders before the read.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".lang de");
    interactive.process("hund bellt laut");
    interactive.process(".name hund fr chien");

    collector.clear();
    interactive.process(R"js(%(do (def go (ev/thread-chan 1)) (def ch (ev/thread-chan 1)))js"
                        R"js( (ev/spawn-thread (ev/take go) (ev/give ch (string (if (zelph/lookup "hund") "de" "") (if (zelph/lookup "chien") "fr" "")))))js"
                        R"js( (zelph/lang "fr") (ev/give go true) (def r (ev/take ch)) (zelph/lang "de") (zelph/out r)))js");
    CHECK(last_out_text(collector) == "fr");
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
        interactive.process(R"js(%(do (def ch (ev/thread-chan 1)) (ev/spawn-thread (def accepted @[]) (each [name f refusal] [["zelph/fact" (fn [] (zelph/fact "ta" "tp" "tb"))] ["zelph/refute" (fn [] (zelph/refute "ta" "tp" "tc"))] ["zelph/negate" (fn [] (zelph/negate nil))] ["zelph/path-guard" (fn [] (zelph/path-guard "ta" "tb"))] ["zelph/list" (fn [] (zelph/list "ta" "tb"))] ["zelph/list-chars" (fn [] (zelph/list-chars "12"))] ["zelph/set" (fn [] (zelph/set "ta"))] ["zelph/collection" (fn [] (zelph/collection "ta"))] ["zelph/conjunction" (fn [] (zelph/conjunction nil nil))] ["zelph/resolve" (fn [] (zelph/resolve "never-named")) "zelph/resolve: 'never-named' has no node, and creating one is main thread only"] ["zelph/var" (fn [] (zelph/var))] ["zelph/query" (fn [] (zelph/query nil))] ["zelph/rule*" (fn [] (zelph/rule* [] nil))] ["zelph/rule" (fn [] (zelph/rule [] nil))] ["zelph/build-rule" (fn [] (zelph/build-rule (fn [] nil)))] ["zelph/dedup-rule" (fn [] (zelph/dedup-rule (fn [] nil)))] ["zelph/rule-text" (fn [] (zelph/rule-text (fn [] nil)))] ["zelph/mention" (fn [] (zelph/mention "ta" "tp" "tb"))] ["zelph/query-text" (fn [] (zelph/query-text (fn [] nil)))] ["zelph/claim-text" (fn [] (zelph/claim-text (fn [] nil)))] ["zelph/operand" (fn [] (zelph/operand (fn [] nil)))] ["zelph/register-keyword" (fn [] (zelph/register-keyword "tkw" (fn [x] x)))] ["zelph/approx" (fn [] (zelph/approx nil "tnet"))] ["zelph/path" (fn [] (zelph/path nil "one-or-more"))] ["zelph/nn-connect" (fn [] (zelph/nn-connect "ta" "tb"))] ["zelph/set-weight" (fn [] (zelph/set-weight "ta" "tb" 1))] ["zelph/nn-compile" (fn [] (zelph/nn-compile [nil nil]))] ["zelph/nn-connect-layers" (fn [] (zelph/nn-connect-layers "ta" "tb"))] ["zelph/set-number-digits" (fn [] (zelph/set-number-digits []))] ["zelph/register-display-scheme" (fn [] (zelph/register-display-scheme "ts" "(" ")"))] ["zelph/set-infix-display" (fn [] (zelph/set-infix-display "ts" []))] ["zelph/set-application-display" (fn [] (zelph/set-application-display "ts" []))] ["zelph/no-selffact-sugar" (fn [] (zelph/no-selffact-sugar "tp"))] ["zelph/import" (fn [] (zelph/import "tfile"))] ["zelph/save" (fn [] (zelph/save "tfile.bin"))] ["zelph/load" (fn [] (zelph/load "tfile.bin"))] ["zelph/run" (fn [] (zelph/run))] ["zelph/run-once" (fn [] (zelph/run-once))] ["zelph/run-delta" (fn [] (zelph/run-delta))] ["zelph/strata" (fn [] (zelph/strata))] ["zelph/rules" (fn [] (zelph/rules))] ["zelph/cluster" (fn [] (zelph/cluster "tc"))] ["zelph/cluster-drop" (fn [] (zelph/cluster-drop "tc"))] ["zelph/clusters" (fn [] (zelph/clusters))] ["zelph/defeated" (fn [] (zelph/defeated))] ["zelph/negation-records" (fn [] (zelph/negation-records true))] ["zelph/auto-run?" (fn [] (zelph/auto-run?))] ["zelph/lang" (fn [] (zelph/lang "de"))] ["zelph/explain" (fn [] (zelph/explain "ta" "tp" "tb"))] ["zelph/prune-fact" (fn [] (zelph/prune-fact "ta" "tp" "tb"))] ["zelph/holders" (fn [] (zelph/holders "ta" "tp" "tb"))] ["zelph/dependents" (fn [] (zelph/dependents "ta" "tp" "tb"))] ["zelph/annotate" (fn [] (zelph/annotate "ta" "tk" "tv"))] ["zelph/unannotate" (fn [] (zelph/unannotate "ta" "tk"))]] (try (do (f) (array/push accepted name)) ([e] (unless (string/find (or refusal (string name ": main thread only")) (string e)) (array/push accepted (string name " -> " e)))))) (ev/give ch (string "accepted: [" (string/join accepted ", ") "]"))) (zelph/out (ev/take ch))))js");
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
