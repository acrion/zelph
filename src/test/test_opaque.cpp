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

#include "capi/zelph_c.h"
#include "network/reasoning.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Mentions and opaque predicates.
//
// A statement claims itself and all facts nested within it, except when a
// nested fact stands in an opaque argument position: in that case, it is
// merely mentioned. `=>` serves as the built-in opaque predicate; a rule's
// ground patterns have always been written down without being claimed,
// under the rule-pattern mark. A mention carries the identical mark, and the
// position of the marked node distinguishes the two. zelph/mention and
// zelph_mention write one directly; `P ~ opaque` makes every argument
// position of P a mention scope.
// ---------------------------------------------------------------------------

namespace
{
    std::vector<std::string> answers_of(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& query)
    {
        collector.clear();
        interactive.process(query);
        return collect_answers(collector);
    }

    std::string explained(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& fact)
    {
        collector.clear();
        interactive.process(".explain " + fact);
        std::string text;
        for (const auto& event : collector.events())
        {
            if (event.channel != zelph::io::OutputChannel::Out) continue;
            std::istringstream lines(event.text);
            for (std::string line; std::getline(lines, line);)
                if (!normalize(line).empty()) text += normalize(line) + "\n";
        }
        return text;
    }
}

TEST_CASE("mention: a mentioned statement answers no query until it is claimed")
{
    // A front end writes "John believes that Mary waits." with the inner
    // statement as a mention: neither a query nor a rule can take `mary
    // waits nil` as data, and zelph/exists must not report it. The rule
    // shows that a condition does not match it either, and that the claim
    // is what lets the rule fire, precisely when the statement is asserted.
    // The rule appears after the mention, ensuring the mention is the one
    // that creates the node.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the fact form")
        {
            interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        }
        SUBCASE("the thunk form")
        {
            interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "mary" "waits" "nil"))))");
        }
        interactive.process("(mary waits nil) => (mary ~ patient)");

        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(janet_value(collector, interactive, R"((zelph/exists "mary" "waits" "nil"))") == "false");
        CHECK(answers_of(collector, interactive, "X ~ patient").empty());

        interactive.process("mary waits nil");
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        CHECK(answers_of(collector, interactive, "X ~ patient") == std::vector<std::string>{"mary ~ patient"}); });
}

TEST_CASE("mention: a collection a mention writes is a value")
{
    // The thunk executes within the scratch cluster associated with a rule
    // build, yet outside its template scope: the collection it writes is
    // not part of a rule's own text -- text that becomes immutable after
    // being written -- so a member may still be added to it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(def held (zelph/mention (fn [] (zelph/fact "john" "owns" (zelph/collection "car2"))))))");
        CHECK(answers_of(collector, interactive, "S owns O").empty());
        CHECK_NOTHROW(interactive.process(R"(%(zelph/fact "car3" "in" (get (get (zelph/parse held) 2) 0)))"));
        (void)collector; });
}

TEST_CASE("mention: a thunk that returns no node, yields or fails is refused and leaves nothing behind")
{
    // The mark starts at the statement the thunk returns, meaning a thunk
    // returning anything else would leave all it wrote claimed: it is
    // refused, and the scratch cluster it wrote into is discarded, exactly
    // as occurs with a thunk that fails or waits. The mention written
    // afterwards shows that the refused call did not leave any rule build
    // open behind it: if one had remained open, the next mention would only
    // build its fact, unmarked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        SUBCASE("a thunk that returns no node")
        {
            CHECK_THROWS_WITH(interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "a" "p" "b") nil)))"),
                              doctest::Contains("zelph/mention: the thunk returned nil, which is not a node"));
        }
        SUBCASE("a thunk that fails")
        {
            CHECK_THROWS_WITH(interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "a" "p" "b") (error "half-built"))))"),
                              doctest::Contains("half-built"));
        }
        SUBCASE("a thunk that yields")
        {
            CHECK_THROWS_WITH(interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "a" "p" "b") (yield 1) (zelph/fact "c" "p" "d"))))"),
                              doctest::Contains("cannot wait for the event loop or yield"));
        }
        SUBCASE("a subject and a predicate without an object")
        {
            CHECK_THROWS_WITH(interactive.process(R"(%(zelph/mention "a" "p"))"),
                              doctest::Contains("zelph/mention: give a thunk that builds the statement, or a subject, a predicate and at least one object"));
        }

        CHECK(answers_of(collector, interactive, "X p Y").empty());
        CHECK(graph->list_clusters().empty());

        interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        CHECK(answers_of(collector, interactive, "X waits nil").empty()); });
}

TEST_CASE("mention: a mention leaves a statement asserted before it asserted")
{
    // What existed before a mention keeps its status: mentioning a claim
    // within the graph must not take the claim back, so that "John believes
    // that Mary waits." does not retract "Mary waits." as previously typed.
    // The nested scenario follows the same principle at a lower level: the
    // thunk reuses the claimed fact.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("mary waits nil");
        interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "john" "says" (zelph/fact "mary" "waits" "nil")))))");

        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        CHECK(answers_of(collector, interactive, "S says O").empty()); });
}

TEST_CASE("mention: a claim after the mention makes it data — typed, Janet block, keyword handler, C API")
{
    // Every writer that asserts claims a mention, just as it claims a rule's
    // ground pattern: the mark is a single mark, and the claim routes are
    // the ones standardized by P-claim. The nested SUBCASE pins that a claim
    // of a statement holding a mention also claims that mention, provided no
    // opaque predicate stands between them.
    SUBCASE("in a session")
    {
        run_both_modes([](auto& collector, auto& interactive)
                       {
            interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
            REQUIRE(answers_of(collector, interactive, "X waits nil").empty());

            SUBCASE("typed")
            {
                interactive.process("mary waits nil");
            }
            SUBCASE("Janet block")
            {
                interactive.process(R"(%(zelph/fact "mary" "waits" "nil"))");
            }
            SUBCASE("keyword handler")
            {
                interactive.process(R"js(%(zelph/register-keyword "claimkw" (fn [t] (zelph/fact "mary" "waits" "nil") nil)))js");
                interactive.process("claimkw");
                interactive.process("");
            }
            SUBCASE("a statement that holds it")
            {
                interactive.process("john says (mary waits nil)");
            }

            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"}); });
    }
    SUBCASE("C API")
    {
        zelph_engine* engine = nullptr;
        REQUIRE(zelph_engine_create([](void*, int32_t, const char*, int32_t) {}, nullptr, &engine) == ZELPH_OK);
        const auto node = [engine](const char* name)
        {
            zelph_node n = 0;
            REQUIRE(zelph_resolve(engine, name, nullptr, &n) == ZELPH_OK);
            return n;
        };
        const zelph_node nil       = node("nil");
        zelph_node       mentioned = 0;
        REQUIRE(zelph_mention(engine, node("mary"), node("waits"), &nil, 1, &mentioned) == ZELPH_OK);

        int32_t exists = -1;
        REQUIRE(zelph_exists(engine, node("mary"), node("waits"), &nil, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);

        zelph_node claimed = 0;
        REQUIRE(zelph_fact(engine, node("mary"), node("waits"), &nil, 1, &claimed) == ZELPH_OK);
        CHECK(claimed == mentioned);
        REQUIRE(zelph_exists(engine, node("mary"), node("waits"), &nil, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
        zelph_engine_destroy(engine);
    }
}

TEST_CASE("mention: a rule zelph/mention writes is not in force")
{
    // A front end writes the statement "John believes that whoever waits
    // sleeps." with the rule as a mention, and zelph/mention returns the
    // `=>` before any statement can hold it. Since no statement named the
    // rule, it was in force immediately upon being written: .list-rules
    // listed it, zelph/rules returned it, and it fired, although a mention
    // claims nothing. The mention marks the `=>` it creates, in both forms
    // and regardless of whether the rule holds a variable or not.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the fact form, ground")
        {
            interactive.process(R"(%(zelph/mention (zelph/mention "ann" "waits" "nil") "=>" (zelph/mention "ann" "sleeps" "nil")))");
        }
        SUBCASE("the fact form, with a variable")
        {
            interactive.process(R"(%(zelph/mention (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X "sleeps" "nil")))");
        }
        SUBCASE("the thunk form, ground")
        {
            interactive.process(R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "ann" "waits" "nil") "=>" (zelph/fact "ann" "sleeps" "nil")))))");
        }
        SUBCASE("the thunk form, with a variable")
        {
            interactive.process(R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X "sleeps" "nil")))))");
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found."));
        CHECK(janet_value(collector, interactive, "(length (zelph/rules))") == "0");

        interactive.process("ann waits nil");
        CHECK(answers_of(collector, interactive, "X sleeps nil").empty()); });
}

TEST_CASE("mention: a mentioned arrow fact that is no rule answers no query")
{
    // The expression `a => b` does not conform to the form of a rule: it is
    // a fact of the relation `=>`, and its mention is akin to that of any
    // other fact. Since the `=>` at the root of a mention remained unmarked,
    // `S => O` answered it as data, contrary to what zelph/mention promises.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the fact form")
        {
            interactive.process(R"(%(zelph/mention "a" "=>" "b"))");
        }
        SUBCASE("the thunk form")
        {
            interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "a" "=>" "b"))))");
        }

        CHECK(answers_of(collector, interactive, "S => O").empty());

        interactive.process("a => b");
        CHECK(answers_of(collector, interactive, "S => O") == std::vector<std::string>{"a => b"}); });
}

TEST_CASE("mention: a rule asserted before its mention stays in force")
{
    // What existed before a mention keeps its status, whether it be a
    // rule or a statement ("mention: a mention leaves a statement asserted
    // before it asserted"). The typed rule built every node the mention
    // names, so the mention creates nothing and marks nothing: the mark
    // goes solely on a `=>` that the mention itself created.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        SUBCASE("the fact form")
        {
            interactive.process(R"(%(zelph/mention (zelph/mention "a" "p" "b") "=>" (zelph/mention "c" "q" "d")))");
        }
        SUBCASE("the thunk form")
        {
            interactive.process(R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))");
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_starts_with(collector, "(a p b) => (c q d)"));

        interactive.process("a p b");
        CHECK(answers_of(collector, interactive, "c q X") == std::vector<std::string>{"c q d"}); });
}

TEST_CASE("mention: a claim of a mentioned rule puts it in force — typed, Janet block, zelph/dedup-rule, zelph/build-rule")
{
    // The claim of a mentioned rule revokes its mark, just as the claim of
    // any mention does. A rule builder asserts the rule it keeps, much
    // like a typed rule statement does: built again by zelph/dedup-rule or
    // zelph/build-rule, the ground text is the mentioned node, hash-consed,
    // and it was returned still marked and never came into force. The rule
    // comes into force between two runs, so a seeded run must supply it with
    // facts older than itself, which check mode verifies.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))");
        process_lines(interactive, "a p b\n.run\n");
        REQUIRE(answers_of(collector, interactive, "c q X").empty());

        SUBCASE("typed")
        {
            interactive.process("(a p b) => (c q d)");
        }
        SUBCASE("Janet block")
        {
            interactive.process(R"(%(zelph/fact (zelph/mention "a" "p" "b") "=>" (zelph/mention "c" "q" "d")))");
        }
        SUBCASE("zelph/dedup-rule")
        {
            interactive.process(R"(%(zelph/dedup-rule (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))");
        }
        SUBCASE("zelph/build-rule")
        {
            interactive.process(R"(%(zelph/build-rule (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))");
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_starts_with(collector, "(a p b) => (c q d)"));

        collector.clear();
        interactive.process(".run-delta");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
        CHECK_FALSE(any_event_contains(collector, "missed by delta seeding"));
        CHECK(answers_of(collector, interactive, "c q X") == std::vector<std::string>{"c q d"});
        CHECK(explained(collector, interactive, "(c q d)") == "c q d\n└─ a p b [axiom]\n"); });
}

TEST_CASE("mention: a typed rule equivalent to a mentioned one with variables is a rule of its own")
{
    // A rule incorporating variables is a new node in each statement
    // that writes it, thus the typed rule does not claim the mentioned one.
    // The duplicate check found the mentioned rule, which was previously in
    // force, as the twin of the typed rule and kept only that one: the
    // mention then became the rule currently in force. Now the typed rule
    // stands as the sole rule in force, and the mention keeps its label.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(def mentioned (zelph/mention (fn [] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X "sleeps" "nil"))))))");
        interactive.process("(X waits nil) => (X sleeps nil)");

        CHECK(janet_value(collector, interactive, "(length (zelph/rules))") == "1");
        CHECK(janet_value(collector, interactive, "(get (zelph/explain mentioned) :status)") == ":rule-mentioned");

        interactive.process("ann waits nil");
        CHECK(answers_of(collector, interactive, "X sleeps nil") == std::vector<std::string>{"ann sleeps nil"}); });
}

TEST_CASE("mention: a rule an opaque holder believes stays out of force, also once the holder is pruned")
{
    // The front end writes "John believes that if a waits then c sleeps." as
    // an opaque fact around a mention of the rule. An opaque holder
    // claims nothing it holds, thus the rule keeps the mention's mark
    // alongside the holder. Absent that mark, the holder was the sole
    // statement naming the rule, and pruning the belief put the rule
    // in force: it fired, as though someone had asserted it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process(R"(%(def belief (zelph/fact "john" "believes" (zelph/mention (fn [] (zelph/rule-text (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))))))");
        interactive.process("a p b");
        REQUIRE(answers_of(collector, interactive, "c q X").empty());

        interactive.process("%(zelph/prune-fact belief)");
        REQUIRE(answers_of(collector, interactive, "S believes O").empty());
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found."));
        CHECK(answers_of(collector, interactive, "c q X").empty()); });
}

TEST_CASE("mention: a question matches a mentioned rule an opaque holder holds by its members")
{
    // The mark keeps a mentioned rule out of force and excluded from every
    // answer as a candidate of its own, yet a question over its holder
    // still reads inside it, just as deep unification examines the interior
    // of any opaque argument ("opaque: deep unification reads inside an
    // opaque argument"). A comma list is compared as a set, regardless of
    // the sequence of its conditions.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("john believes ((tom waits nil, ann waits nil) => (bob sleeps nil))");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found."));

        CHECK(answers_of(collector, interactive, "X believes ((tom waits nil, ann waits nil) => (bob sleeps nil))").size() == 1);
        CHECK(answers_of(collector, interactive, "X believes ((ann waits nil, tom waits nil) => (bob sleeps nil))").size() == 1);
        CHECK(answers_of(collector, interactive, "X believes ((ann waits nil, tom waits nil) => (bob sleeps sun))").empty()); });
}

TEST_CASE("mention: a refuted statement can be mentioned and stays refuted")
{
    // "John believes that Mary waits." after "Mary does not wait." mentions
    // a statement that the graph refutes. Constructed using zelph/fact, the
    // mention was refused as known to be wrong; a mention keeps what
    // existed, thus the refutation remains intact and nothing is marked
    // over it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("¬(mary waits nil)");

        SUBCASE("the fact form")
        {
            interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        }
        SUBCASE("the thunk form")
        {
            interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "john" "says" (zelph/fact "mary" "waits" "nil")))))");
        }

        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(explained(collector, interactive, "mary waits nil") == "¬(mary waits nil) [axiom]\n");
        CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("known to be wrong")); });
}

TEST_CASE("mention: refuting a statement that is only mentioned claims its negation")
{
    // A mention is not a claim, thus "¬" can refute it, just as it
    // refutes a rule's ground pattern: the refutation constitutes the
    // claim, and from that point onward the statement is refused as known
    // to be wrong. Refused as known to be true, a mention would make
    // every later denial impossible.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        CHECK_NOTHROW(interactive.process("¬(mary waits nil)"));

        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(explained(collector, interactive, "mary waits nil") == "¬(mary waits nil) [axiom]\n");
        CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("known to be wrong")); });
}

TEST_CASE("mention: a rule refuted before anybody wrote it is refused as known to be wrong when typed")
{
    // A refutation writes the fact it creates as a mention (Zelph::refute),
    // and also treats a rule in the same manner, ensuring the rule carries
    // the mark of a mention. A typed rule claims it, and a claim of a
    // refuted statement is refused, just as for any refuted fact. Unmarked,
    // the typed rule was accepted without comment, printed as its
    // refutation, and stayed out of force.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("¬((a p b) => (c q d))");
        CHECK_THROWS_WITH(interactive.process("(a p b) => (c q d)"), doctest::Contains("known to be wrong"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found.")); });
}

TEST_CASE("mention: a refutation marking inside a mention refutes nothing")
{
    // "John believes that Mary does not wait." mentions a refutation; it
    // must not itself serve as a refutation. Outside the template scope, a
    // marking takes effect immediately, thereby concealing `mary waits nil`
    // from every query before anyone had denied it. Claiming the marking
    // later applies it, just as with a rule's pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "john" "says" (zelph/fact (zelph/fact "mary" "waits" "nil") "~" "refuted")))))");
        CHECK(answers_of(collector, interactive, "X ~ refuted").empty());

        SUBCASE("the statement can still be asserted")
        {
            CHECK_NOTHROW(interactive.process("mary waits nil"));
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        }
        SUBCASE("claiming the marking refutes it")
        {
            interactive.process("(mary waits nil) ~ refuted");
            CHECK(answers_of(collector, interactive, "X ~ refuted") == std::vector<std::string>{"(mary waits nil) ~ refuted"});
            CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("known to be wrong"));
        } });
}

TEST_CASE("mention: a cluster drop restores the mark without recording it in the outer cluster")
{
    // A front end's dry run claims inside a scratch cluster and drops it; a
    // mention that was claimed there must be a mention again afterwards, and
    // the restored mark must not be associated with any cluster, otherwise
    // dropping the outer cluster later erased it and a .save/.load read the
    // mention as a claim (P-restore-mark, shown here for the mark a mention
    // carries).
    const auto file = std::filesystem::temp_directory_path() / "zelph_opaque_restored_mention.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        process_lines(interactive, ".cluster outer\n.cluster exp\nmary waits nil\n.cluster outer\n.cluster-drop exp");
        CHECK(graph->list_clusters() == std::vector<std::pair<std::string, size_t>>{{"outer", 0}});
        CHECK(answers_of(collector, interactive, "X waits nil").empty());

        process_lines(interactive, ".cluster default\n.cluster-drop outer");
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(answers_of(collector, interactive, "X waits nil").empty()); });
    std::filesystem::remove(file);
}

TEST_CASE("mention: a cluster drop takes back a claim of a mentioned rule, and a mention written in the cluster")
{
    // During a dry run at the front end, a claim originating within a
    // cluster is made and subsequently discarded. The claim of a mentioned
    // rule revoked a mark the rule had before the cluster, so the drop
    // restores that mark, and the rule is out of force again. A mention
    // that the cluster writes takes the rule and its mark along, and the
    // same rule typed afterwards is a rule in force, just like any other.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        constexpr const char* mention = R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))";
        const auto            listed  = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return !any_output_contains(collector, "No rules found.");
        };

        SUBCASE("a claim made in the cluster")
        {
            interactive.process(mention);
            process_lines(interactive, ".cluster exp\n(a p b) => (c q d)\n");
            REQUIRE(listed());
            process_lines(interactive, ".cluster default\n.cluster-drop exp\n");
            CHECK_FALSE(listed());

            interactive.process("a p b");
            CHECK(answers_of(collector, interactive, "c q X").empty());
        }
        SUBCASE("a mention written in the cluster")
        {
            interactive.process(".cluster exp");
            interactive.process(mention);
            CHECK_FALSE(listed());
            process_lines(interactive, ".cluster default\n.cluster-drop exp\n");

            process_lines(interactive, "(a p b) => (c q d)\na p b\n");
            CHECK(answers_of(collector, interactive, "c q X") == std::vector<std::string>{"c q d"});
        } });
}

// A fact corresponds to the hash of its triple: if a mention is removed while
// its components remain intact, the same statement entered or derived later
// shares the identical identifier. The mark remained tied to the identifier,
// so a rule that derived `t p k` after a dropped cluster had mentioned it
// wrote a pattern that answered no query until a later run claimed it, and
// check mode reported the derivation that the delta had missed. The fact that
// a refutation creates is a mention as well (Zelph::refute), which a dry run
// within a dropped cluster reaches in the same manner as a front end does.
TEST_CASE("mention: a fact derived after its mention was removed is data")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "t go k\np q r\n");
        SUBCASE("a mention a dropped cluster wrote")
        {
            process_lines(interactive, ".cluster c\n%(zelph/mention \"t\" \"p\" \"k\")\n.cluster default\n.cluster-drop c\n");
        }
        SUBCASE("a refutation a dropped cluster made")
        {
            process_lines(interactive, ".cluster c\n¬(t p k)\n.cluster default\n.cluster-drop c\n");
        }
        SUBCASE("a mention removed with .remove")
        {
            interactive.process(R"(%(zelph/mention "t" "p" "k"))");
            collector.clear();
            interactive.process(".node t p k");
            std::string id;
            for (const auto& event : collector.events())
                if (const std::size_t at = event.text.find("Node ID: "); at != std::string::npos) id = event.text.substr(at + 9);
            REQUIRE_FALSE(id.empty());
            interactive.process(".remove " + id);
        }

        interactive.process("(X go Y) => (X p Y)");
        CHECK(answers_of(collector, interactive, "S p O") == std::vector<std::string>{"t p k"}); });
}

TEST_CASE("mention: the marks survive .save/.load and a predicate slice")
{
    // The mark constitutes a fact, meaning a whole-file round trip keeps
    // a mention as a mention and its claim remains possible afterwards. A
    // slice carries the claimed facts of its predicates; a mention is not a
    // claimed fact and remains excluded, just as a rule's pattern does,
    // instead of being received as data.
    namespace fs    = std::filesystem;
    const auto full = fs::temp_directory_path() / "zelph_opaque_mention_full.bin";
    const auto cut  = fs::temp_directory_path() / "zelph_opaque_mention_slice.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(R"(%(zelph/mention "mary" "waits" "nil"))");
        interactive.process("bob waits nil");
        interactive.process(".save \"" + full.string() + "\"");
        interactive.process(".save-predicates \"" + cut.string() + "\" waits");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    SUBCASE(".save/.load")
    {
        interactive.process(".load \"" + full.string() + "\"");
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"bob waits nil"});
        interactive.process("mary waits nil");
        CHECK(answers_of(collector, interactive, "X waits nil").size() == 2);
    }
    SUBCASE("a predicate slice")
    {
        interactive.process(".load \"" + cut.string() + "\"");
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"bob waits nil"});
    }

    fs::remove(full);
    fs::remove(cut);
}

TEST_CASE("mention: a rule zelph/mention wrote stays out of force across .save/.load")
{
    // The mark attached to a rule that has been mentioned is a fact like the
    // mark on any mention, saved with the graph and read back into the index
    // by a load, ensuring the rule reappears as a mention and a claim still
    // puts it in force. A rule that was mentioned and saved without a mark
    // reemerged in force.
    namespace fs    = std::filesystem;
    const auto file = fs::temp_directory_path() / "zelph_opaque_mentioned_rule.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(R"(%(zelph/mention (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")))))");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    process_lines(interactive, "a p b\n.run\n");

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found."));
    CHECK(answers_of(collector, interactive, "c q X").empty());
    CHECK(explained(collector, interactive, "((a p b) => (c q d))") == "(a p b) => (c q d) [rule mentioned; not in force]\n");

    process_lines(interactive, "(a p b) => (c q d)\n.run\n");
    CHECK(answers_of(collector, interactive, "c q X") == std::vector<std::string>{"c q d"});

    fs::remove(file);
}

TEST_CASE("explain: a mention is labelled by the position it stands in")
{
    // A rule's ground pattern and a mention carry the same mark, and the
    // label has to indicate whether a node is the one a rule authored or the
    // one a statement wrote down: "[rule pattern; not asserted]" for a node
    // a rule wrote, "[mentioned; not asserted]" for a node a statement wrote
    // down. The walk proceeds upward through marked holders, so a pattern
    // nested deeper within a rule stays a rule pattern, and a rule a mention
    // holds is rule text.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process("((a p b) q c) => (d e f)");
        interactive.process("(X p k) => (X likes (k is green))");
        interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "john" "says" (zelph/fact "mary" "waits" "nil")))))");
        interactive.process(R"(%(zelph/mention (fn [] (zelph/fact "sue" "says" (zelph/rule-text (fn [] (zelph/fact (zelph/fact "g" "h" "i") "=>" (zelph/fact "j" "k" "l"))))))))");

        CHECK(explained(collector, interactive, "a p b") == "a p b [rule pattern; not asserted]\n");
        CHECK(explained(collector, interactive, "d e f") == "d e f [rule pattern; not asserted]\n");
        CHECK(explained(collector, interactive, "john says (mary waits nil)") == "john says (mary waits nil) [mentioned; not asserted]\n");
        CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n");
        CHECK(explained(collector, interactive, "g h i") == "g h i [rule pattern; not asserted]\n");
        CHECK(explained(collector, interactive, "k is green") == "k is green [rule pattern; not asserted]\n");

        SUBCASE(".node")
        {
            collector.clear();
            interactive.process(".node mary waits nil");
            CHECK(any_output_contains(collector, "Mentioned (not asserted): yes"));
            CHECK_FALSE(any_output_contains(collector, "Rule pattern (not asserted)"));

            collector.clear();
            interactive.process(".node a p b");
            CHECK(any_output_contains(collector, "Rule pattern (not asserted): yes"));
        }
        SUBCASE("zelph/explain")
        {
            CHECK(janet_value(collector, interactive, R"((get (zelph/explain "mary" "waits" "nil") :status))") == ":mentioned");
            CHECK(janet_value(collector, interactive, R"((get (zelph/explain "a" "p" "b") :status))") == ":rule-pattern");
        } });
}

// ---------------------------------------------------------------------------
// The declaration `P ~ opaque`.
// ---------------------------------------------------------------------------

TEST_CASE("opaque: a statement claims nothing in the argument positions of an opaque predicate")
{
    // A program writes "John believes that Mary waits." as a single
    // claimed fact of `believes` around a mention. zelph/fact and
    // zelph_fact claim the facts nested in what they assert, and thus they
    // claimed the mention: `mary waits nil` answered as if Mary were
    // waiting. Every invocation is a statement of its own, so a nested fact
    // built by its own zelph/fact call remains the claim of that call and
    // persists as such.
    SUBCASE("in a session")
    {
        run_both_modes([](auto& collector, auto& interactive)
                       {
            interactive.process("believes ~ opaque");

            SUBCASE("zelph/fact over a mention")
            {
                interactive.process(R"(%(zelph/fact "john" "believes" (zelph/mention "mary" "waits" "nil")))");
                CHECK(answers_of(collector, interactive, "X waits nil").empty());
                CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
            }
            SUBCASE("a nested fact claimed by its own call stays claimed")
            {
                interactive.process(R"(%(zelph/fact "john" "believes" (zelph/fact "mary" "waits" "nil")))");
                CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
            }
            SUBCASE("a predicate that is not opaque claims the mention, as before")
            {
                interactive.process(R"(%(zelph/fact "john" "says" (zelph/mention "mary" "waits" "nil")))");
                CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
            } });
    }
    SUBCASE("C API")
    {
        zelph_engine* engine = nullptr;
        REQUIRE(zelph_engine_create([](void*, int32_t, const char*, int32_t) {}, nullptr, &engine) == ZELPH_OK);
        const auto node = [engine](const char* name, const char* lang)
        {
            zelph_node n = 0;
            REQUIRE(zelph_resolve(engine, name, lang, &n) == ZELPH_OK);
            return n;
        };
        const zelph_node opaque      = node("opaque", "zelph");
        zelph_node       declaration = 0;
        REQUIRE(zelph_fact(engine, node("believes", nullptr), node("~", nullptr), &opaque, 1, &declaration) == ZELPH_OK);

        const zelph_node nil       = node("nil", nullptr);
        zelph_node       mentioned = 0;
        REQUIRE(zelph_mention(engine, node("mary", nullptr), node("waits", nullptr), &nil, 1, &mentioned) == ZELPH_OK);
        zelph_node holder = 0;
        REQUIRE(zelph_fact(engine, node("john", nullptr), node("believes", nullptr), &mentioned, 1, &holder) == ZELPH_OK);

        int32_t exists = -1;
        REQUIRE(zelph_exists(engine, node("john", nullptr), node("believes", nullptr), &mentioned, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
        REQUIRE(zelph_exists(engine, node("mary", nullptr), node("waits", nullptr), &nil, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);
        zelph_engine_destroy(engine);
    }
}

TEST_CASE("opaque: a typed statement writes its nested statement as a mention")
{
    // "John believes (Mary waits)" typed following the declaration: the
    // parser encloses every nested argument within a mention scope, just
    // as it encloses a nested `=>` within rule text, and the claim of the
    // whole statement stops at the argument. Absent this, the typed
    // statement would have claimed `mary waits nil`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");

        SUBCASE("a fact")
        {
            interactive.process("john believes (mary waits nil)");
            CHECK(answers_of(collector, interactive, "X waits nil").empty());
            CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
            CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n");
        }
        SUBCASE("a statement asserted before keeps its claim")
        {
            interactive.process("mary waits nil");
            interactive.process("john believes (mary waits nil)");
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        }
        SUBCASE("a set constant of statements")
        {
            interactive.process("john believes {(a p b) (c q d)}");
            CHECK(answers_of(collector, interactive, "S p O").empty());
            CHECK(answers_of(collector, interactive, "S q O").empty());
        }
        SUBCASE("inside a statement that claims")
        {
            // The outer statement claims its opaque fact and ceases at
            // that point.
            interactive.process("sue says (john believes (mary waits nil))");
            CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
            CHECK(answers_of(collector, interactive, "X waits nil").empty());
        }
        SUBCASE("a refutation marking in the argument refutes nothing")
        {
            // "John believes that Bob does not wait." Outside a mention, the
            // nested marking was applied and un-claimed a claimed fact.
            interactive.process("bob waits nil");
            interactive.process("john believes ((bob waits nil) ~ refuted)");
            interactive.process("john believes ((sue waits nil) ~ refuted)");
            CHECK(answers_of(collector, interactive, "X ~ refuted").empty());
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"bob waits nil"});
            CHECK_NOTHROW(interactive.process("sue waits nil"));
        }
        // An opaque argument functions as a mention scope, and a focus
        // within it wrote a claim beside the part it hands on:
        // `sue says (mary waits nil)` answered as an axiom.
        SUBCASE("a fact a focus writes inside the argument")
        {
            interactive.process("john believes (sue says *(mary waits nil))");
            CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
            CHECK(answers_of(collector, interactive, "S says O").empty());
            CHECK(answers_of(collector, interactive, "X waits nil").empty());
            CHECK(explained(collector, interactive, "sue says (mary waits nil)") == "sue says (mary waits nil) [mentioned; not asserted]\n");
        }
        SUBCASE("a later claim makes the mention data")
        {
            interactive.process("john believes (mary waits nil)");
            interactive.process("mary waits nil");
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
            CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
        }
        // A fact that a focus writes on a separate line is claimed as the
        // line of that fact claims it: following the mention, the line
        // "sue says *(mary waits nil)" left `sue says (mary waits nil)`
        // mentioned.
        SUBCASE("a fact a focus writes later makes the mention data")
        {
            interactive.process("john believes (sue says (mary waits nil))");
            interactive.process("sue says *(mary waits nil)");
            CHECK(answers_of(collector, interactive, "S says O") == std::vector<std::string>{"sue says (mary waits nil)"});
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        } });
}

TEST_CASE("opaque: the subject of an opaque fact is a mention scope as well")
{
    // Each argument position of an opaque predicate, including the
    // subject, is a mention scope: in the sentence "That Mary waits
    // worries Sue." the entire statement serves as the subject. Every
    // writer must handle that position in the same manner as it does the
    // objects -- the parser wraps it, the claim of a Janet call passes
    // it, and a firing marks what it creates there.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("worries ~ opaque");

        SUBCASE("typed")
        {
            interactive.process("(mary waits nil) worries sue");
        }
        SUBCASE("a Janet call")
        {
            interactive.process(R"(%(zelph/fact (zelph/mention "mary" "waits" "nil") "worries" "sue"))");
        }
        SUBCASE("a firing")
        {
            interactive.process("(X ~ woman) => ((X waits nil) worries sue)");
            interactive.process("mary ~ woman");
        }

        CHECK(answers_of(collector, interactive, "S worries O") == std::vector<std::string>{"(mary waits nil) worries sue"});
        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n"); });
}

TEST_CASE("opaque: a typed refutation of an opaque fact claims nothing it holds")
{
    // "John does not believe that Bob runs." refutes the belief, and the
    // parser wraps the arguments of a refuted statement in the same
    // manner as it wraps those of an asserted statement: the refutation
    // says nothing about Bob, who might still be said to run, while the
    // belief stays refuted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("¬(john believes (bob runs nil))");

        CHECK(answers_of(collector, interactive, "X runs nil").empty());
        CHECK(explained(collector, interactive, "bob runs nil") == "bob runs nil [mentioned; not asserted]\n");
        CHECK(explained(collector, interactive, "john believes (bob runs nil)") == "¬(john believes (bob runs nil)) [axiom]\n");

        CHECK_NOTHROW(interactive.process("bob runs nil"));
        CHECK(answers_of(collector, interactive, "X runs nil") == std::vector<std::string>{"bob runs nil"});
        CHECK_THROWS_WITH(interactive.process("john believes (bob runs nil)"), doctest::Contains("known to be wrong")); });
}

TEST_CASE("opaque: a firing writes a mention where its consequence has an opaque predicate")
{
    // "Every woman believes that Bob waits." A firing claims what its
    // consequence writes, so the derived belief asserted `bob waits nil`
    // and `tom sleeps nil`. The ground portion was the rule's pattern
    // before and stays unclaimed; the part the binding completes is created
    // by the firing and is marked as a mention, consistent with how the
    // parts of a rule that a consequence mentions are treated.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("(X ~ woman) => (X believes (bob waits nil))");
        interactive.process("(X ~ man, Y ~ woman) => (Y believes (X sleeps nil))");
        interactive.process("tom ~ man");
        interactive.process("mary ~ woman");

        const auto beliefs = answers_of(collector, interactive, "S believes O");
        CHECK(beliefs.size() == 2);
        CHECK(std::find(beliefs.begin(), beliefs.end(), "mary believes (bob waits nil)") != beliefs.end());
        CHECK(std::find(beliefs.begin(), beliefs.end(), "mary believes (tom sleeps nil)") != beliefs.end());
        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(answers_of(collector, interactive, "X sleeps nil").empty());
        CHECK(explained(collector, interactive, "tom sleeps nil") == "tom sleeps nil [mentioned; not asserted]\n"); });
}

TEST_CASE("opaque: a rule a firing writes in an opaque argument stays out of force when its holder goes")
{
    // "Every woman believes that she sleeps when she waits." The firing
    // writes the rule as a mention, just as the parser writes the rule of a
    // typed belief, so a derived holder and a typed one leave the rule in
    // the same state. Unmarked, the rule was out of force solely while its
    // holder was present: once the premise and the belief were pruned, it
    // came into force and derived `mary sleeps nil`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "believes ~ opaque\n(X ~ woman) => (X believes ((X waits nil) => (X sleeps nil)))\nmary ~ woman\nmary waits nil\n");
        REQUIRE(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"mary believes ((mary waits nil) => (mary sleeps nil))"});
        REQUIRE(answers_of(collector, interactive, "X sleeps nil").empty());

        interactive.process(R"(%(zelph/prune-fact "mary" "~" "woman"))");
        interactive.process(".prune-facts mary believes ((mary waits nil) => (mary sleeps nil))");
        REQUIRE(answers_of(collector, interactive, "S believes O").empty());
        interactive.run(true, false, false);

        CHECK(answers_of(collector, interactive, "X sleeps nil").empty());
        CHECK(janet_value(collector, interactive, "(length (zelph/rules))") == "1"); });
}

TEST_CASE("opaque: a firing writes a refutation or a refuted fact in an opaque argument as a mention")
{
    // An opaque argument is a mention, which claims nothing and refutes
    // nothing, as is the case with a typed statement
    // ("opaque: a typed statement writes its nested statement as a mention").
    // The prediction of a firing ignored opacity: both a refutation marking of
    // a claimed fact within the argument and a refuted fact there were
    // reported as contradictions at every run, and a refuted fact the binding
    // completes would have been constructed at probability 1, thereby
    // terminating the run.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        std::string believed;
        bool        refuted = true;
        SUBCASE("a marking of a claimed fact")
        {
            process_lines(interactive, "t p b\nt go k\n(X go Y) => (k believes ((t p b) ~ refuted))\n");
            believed = "k believes ((t p b) ~ refuted)";
            refuted  = false;
        }
        SUBCASE("a marking of a claimed fact a binding completes")
        {
            process_lines(interactive, "t p b\nt go refuted\n(X go Y) => (k believes ((t p b) ~ Y))\n");
            believed = "k believes ((t p b) ~ refuted)";
            refuted  = false;
        }
        SUBCASE("a refuted fact")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k believes (t p b))\n");
            believed = "k believes (t p b)";
        }
        SUBCASE("a refuted fact the binding completes")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k believes (X p b))\n");
            believed = "k believes (t p b)";
        }
        SUBCASE("a marking of a refuted fact")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k believes ((t p b) ~ refuted))\n");
            believed = "k believes ((t p b) ~ refuted)";
        }
        SUBCASE("a marking of a claimed fact below an opaque fact the consequence nests")
        {
            process_lines(interactive, "t p b\nt go k\n(X go Y) => (sue says (k believes ((t p b) ~ refuted)))\n");
            believed = "k believes ((t p b) ~ refuted)";
            refuted  = false;
        }
        SUBCASE("a refuted fact the binding completes below an opaque fact the consequence nests")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (sue says (k believes (X p b)))\n");
            believed = "k believes (t p b)";
        }
        // A member of a set within the argument is mentioned as well. The
        // prediction did not enter the set, thus the instantiation built
        // the fact at probability 1, which ended the run with "known to be
        // wrong".
        SUBCASE("a refuted fact the binding completes in a set the argument holds")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k believes {(X p b)})\n");
            believed = "k believes {(t p b)}";
        }
        // The opaque predicate corresponds to the value of a binding, and
        // during the walk, this value is read exactly as the firing writes
        // it.
        SUBCASE("a marking of a claimed fact below the opaque predicate a binding supplies")
        {
            process_lines(interactive, "t p b\nt go believes\n(X go Y) => (sue says (k Y ((t p b) ~ refuted)))\n");
            believed = "k believes ((t p b) ~ refuted)";
            refuted  = false;
        }
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_FALSE(has_contradiction(collector));

        CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{believed});
        if (refuted)
        {
            CHECK(answers_of(collector, interactive, "X ~ refuted") == std::vector<std::string>{"(t p b) ~ refuted"});
            CHECK_THROWS_WITH(interactive.process("t p b"), doctest::Contains("known to be wrong"));
        }
        else
        {
            CHECK(answers_of(collector, interactive, "S p O") == std::vector<std::string>{"t p b"});
            CHECK(answers_of(collector, interactive, "X ~ refuted").empty());
        } });
}

TEST_CASE("opaque: a firing writes a mention below an opaque fact nested in its consequence")
{
    // "Sue says that every woman believes that she waits." The consequence
    // is a `says` fact, which claims what it holds, all the way down to
    // the nested `believes` fact, whose argument is an opaque position
    // just like any other: the typed statement leaves `ann waits nil` a
    // mention there. The firing marked only the arguments of the
    // consequence's own predicate, thus the instance created inside the
    // nested belief came out claimed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("(X ~ woman) => (sue says (X believes (X waits nil)))");
        interactive.process("mary ~ woman");
        interactive.process("sue says (ann believes (ann waits nil))");

        const auto said = answers_of(collector, interactive, "S says O");
        CHECK(said.size() == 2);
        CHECK(std::find(said.begin(), said.end(), "sue says (mary believes (mary waits nil))") != said.end());
        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n");
        CHECK(explained(collector, interactive, "ann waits nil") == "ann waits nil [mentioned; not asserted]\n"); });
}

TEST_CASE("opaque: a firing writes a mention when its predicate was declared earlier in the same run")
{
    // A rule featuring a variable predicate writes a belief only once
    // `believes` is an attitude, which another firing within the same run
    // derives one step after the firing that declared `believes` opaque.
    // Whether a consequence's predicate is opaque was decided once per
    // consequence and run, at the moment of its first match, `says`, before
    // anything was declared: the later belief kept that answer, and what it
    // holds came out claimed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        process_lines(interactive,
                      "says ~ attitude\n"
                      "believes ~ modal\n"
                      "(P ~ modal) => (P ~ declarable)\n"
                      "(P ~ declarable) => (P ~ opaque)\n"
                      "(P ~ opaque) => (P ~ attitude)\n"
                      "(X ~ man, Y ~ woman, R ~ attitude) => (Y R (X sleeps R))\n"
                      "tom ~ man\n"
                      "mary ~ woman\n"
                      ".run");

        REQUIRE(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"believes ~ opaque"});
        CHECK(answers_of(collector, interactive, "X sleeps Y") == std::vector<std::string>{"tom sleeps says"});
        CHECK(explained(collector, interactive, "tom sleeps believes") == "tom sleeps believes [mentioned; not asserted]\n"); });
}

TEST_CASE("opaque: deep unification reads inside an opaque argument")
{
    // "If John believes that Mary waits then ..." matches the stored
    // mention by its structure, just as a condition over a held rule reads
    // inside `=>`. The mark keeps the mention from becoming data, yet it
    // must not keep a condition from reading it where it stands.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("(Z believes (Y waits nil)) => (Z ~ credulous)");
        interactive.process("john believes (mary waits nil)");

        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(answers_of(collector, interactive, "X ~ credulous") == std::vector<std::string>{"john ~ credulous"});
        CHECK(answers_of(collector, interactive, "S believes (Y waits nil)") == std::vector<std::string>{"john believes (mary waits nil)"}); });
}

TEST_CASE("opaque: a set pattern matches a box of mentions by its members")
{
    // A front end writes a that-clause comprising multiple conditions in
    // the form of a box: a set constant whose members are mentions. A
    // condition or a question featuring a set pattern reads such a box
    // member by member, just as it reads a set constant of claims, with
    // the members remaining mentions throughout.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("john believes {(a p b) (c q d)}");

        SUBCASE("a condition")
        {
            interactive.process("(Z believes {(X p b) (c q d)}) => (X ~ found)");
            CHECK(answers_of(collector, interactive, "X ~ found") == std::vector<std::string>{"a ~ found"});
        }
        SUBCASE("a question")
        {
            CHECK(answers_of(collector, interactive, "Z believes {(X p b) (c q d)}").size() == 1);
        }

        CHECK(answers_of(collector, interactive, "X p b").empty());
        CHECK(explained(collector, interactive, "a p b") == "a p b [mentioned; not asserted]\n"); });
}

TEST_CASE("opaque: a condition over an opaque argument does not make it data")
{
    // A rule that reads a belief does not assert what is believed:
    // regardless of whether the statement or the rule comes first, neither
    // writing the rule nor its firing claims the mention.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");

        SUBCASE("the statement first")
        {
            interactive.process("john believes (mary waits nil)");
            interactive.process("(john believes (mary waits nil)) => (john ~ hopeful)");
        }
        SUBCASE("the rule first")
        {
            interactive.process("(john believes (mary waits nil)) => (john ~ hopeful)");
            interactive.process("john believes (mary waits nil)");
        }

        interactive.process("(Z believes (Y waits nil)) => (Y ~ awaited)");
        CHECK(answers_of(collector, interactive, "X ~ hopeful") == std::vector<std::string>{"john ~ hopeful"});
        CHECK(answers_of(collector, interactive, "X ~ awaited") == std::vector<std::string>{"mary ~ awaited"});
        CHECK(answers_of(collector, interactive, "X waits nil").empty()); });
}

TEST_CASE("opaque: a rule that binds a whole mention keeps it a mention, a rule that derives it makes it data")
{
    // "Whatever John believes is a belief." binds the mention itself and
    // writes a fact concerning it. The binding supplied that value, and a
    // derived fact claims nothing that only its binding supplied. "Whatever
    // John believes about waiting is true." derives the believed statement
    // as its consequence. A derived statement functions as a claim,
    // regardless of whether its node existed before or not. A front end
    // relies on both: rules concerning beliefs must not assert them,
    // whereas a rule that asserts them must.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("john believes (mary waits nil)");

        SUBCASE("a rule that binds the mention")
        {
            interactive.process("(X believes Y) => (Y ~ belief)");
            CHECK(answers_of(collector, interactive, "X ~ belief") == std::vector<std::string>{"(mary waits nil) ~ belief"});
            CHECK(answers_of(collector, interactive, "X waits nil").empty());
            CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n");
        }
        SUBCASE("a rule that derives what is believed")
        {
            interactive.process("(john believes (Y waits nil)) => (Y waits nil)");
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        } });
}

TEST_CASE("opaque: declaring a predicate opaque is refused while one of its facts holds a statement")
{
    // A statement typed before the declaration claimed what it nests, and
    // the marks are unable to identify which of those nested facts was
    // claimed by P alone, thus a late declaration would alter prior claims
    // silently. The refusal names the predicate and one such fact; a
    // program maps it using the fixed part preceding " -- ". Facts whose
    // arguments are atoms do not block it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("john knows mary");
        CHECK_NOTHROW(interactive.process("knows ~ opaque"));

        interactive.process("john believes (mary waits nil)");
        CHECK_THROWS_WITH_AS(interactive.process("believes ~ opaque"),
                             doctest::Contains("fact(): a predicate cannot be declared opaque while one of its facts holds a statement -- believes: john believes (mary waits nil)"),
                             std::runtime_error);
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});

        SUBCASE("a core predicate is refused")
        {
            CHECK_THROWS_WITH_AS(interactive.process("~ ~ opaque"), doctest::Contains("a core predicate cannot be declared opaque"), std::runtime_error);
        }
        SUBCASE("a firing that would declare it reports the refusal")
        {
            interactive.process("believes ~ hesitant");
            interactive.process(".semi-naive on");
            collector.clear();
            interactive.process("(P ~ hesitant) => (P ~ opaque)");
            CHECK(any_output_contains(collector, "a predicate cannot be declared opaque while one of its facts holds a statement"));
            CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"knows ~ opaque"});
        }
        SUBCASE("a declaration a rule only wrote down is refused when a firing claims it")
        {
            interactive.process(".semi-naive on");
            collector.clear();
            interactive.process("(x y z) => (believes ~ opaque)");
            interactive.process("x y z");
            CHECK(any_output_contains(collector, "a predicate cannot be declared opaque while one of its facts holds a statement"));
            CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"knows ~ opaque"});
        }
        SUBCASE("removing the declaration is refused likewise")
        {
            interactive.process("john knows (sue runs nil)");
            const std::string refusal = "a predicate's opacity cannot be removed while one of its facts holds a statement -- knows: john knows (sue runs nil)";
            CHECK_THROWS_WITH_AS(interactive.process(".prune-facts knows ~ opaque"), doctest::Contains(refusal), std::runtime_error);
            CHECK_THROWS_WITH_AS(interactive.process(".prune-facts P ~ opaque"), doctest::Contains(refusal), std::runtime_error);
            // Taking the node `opaque` takes every declaration with it. The
            // query below also shows that the refusal ended the pruning
            // run: a prune mode left enabled makes each query gather
            // removal targets rather than answer.
            CHECK_THROWS_WITH_AS(interactive.process(".prune-nodes knows ~ O"), doctest::Contains(refusal), std::runtime_error);
            CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"knows ~ opaque"});
            CHECK(answers_of(collector, interactive, "X runs nil").empty());
        }
        SUBCASE("removing it with every fact that holds a statement is allowed")
        {
            interactive.process(".prune-facts knows ~ opaque");
            CHECK(answers_of(collector, interactive, "P ~ opaque").empty());
        }
        // Merging `believes` with the declared `knows`, in either direction,
        // results in a single node that carries the declaration and the fact
        // holding a claimed statement. The merge succeeded: the survivor
        // became opaque, containing `mary waits nil` claimed within it, and
        // its declaration could no longer be removed. The refusal occurs
        // before any alteration, thus preserving the names and the facts in
        // their original state.
        SUBCASE("a name merge that would make it opaque is refused likewise")
        {
            const std::string refusal = "fact(): a predicate cannot be declared opaque while one of its facts holds a statement -- believes: john believes (mary waits nil)";
            SUBCASE("the declared predicate merged into it")
            {
                CHECK_THROWS_WITH_AS(interactive.process(".name believes knows"), doctest::Contains(refusal), std::runtime_error);
            }
            SUBCASE("it merged into the declared predicate")
            {
                CHECK_THROWS_WITH_AS(interactive.process(".name knows believes"), doctest::Contains(refusal), std::runtime_error);
            }
            CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"knows ~ opaque"});
            CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
            CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});
        } });
}

// The node `opaque` keeps its name in the language zelph and does not merge
// with any other node, just as each node of the engine's vocabulary does
// (test_name_merge.cpp, "name merge: a name of the engine's vocabulary stays
// with its node"). The first three scenarios listed below were SUBCASEs of
// the prior case, under "a name merge that would make it opaque is refused
// likewise", with the names "the node opaque merged into its class", "its
// class merged into the node opaque under a name in another language", and
// "the node opaque merged into a class without a name in zelph". They pinned
// the opacity refusal for merging the node `opaque` with the class
// `attitude`, which caused the claimed `believes ~ attitude` to become a
// declaration while `john believes (mary waits nil)` held a claimed
// statement. Such a merge is now refused whatever the facts hold, with the
// vocabulary's message, before any fact is read: the opacity refusal named a
// fact as the obstacle, as though the merge could occur in its absence. The
// remaining scenarios removed the name via a rename or `.delname`, or gave it
// to the class while no node carried it. The declarations lapsed, or
// `believes ~ attitude` turned into one, and `sue believes (bob runs nil)`
// claimed `bob runs nil`, without encountering the refusal that arises from
// removing or making a declaration.
TEST_CASE("opaque: the name opaque stays with its node, and that node merges with no other")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "believes ~ attitude\njohn believes (mary waits nil)");
        const std::string given = "Requested name 'opaque' belongs to the engine's vocabulary in language 'zelph'";
        const std::string merge = "a node of the engine's vocabulary merges with no other";

        SUBCASE("given to the class while no node carries it")
        {
            CHECK_THROWS_WITH_AS(interactive.process(".name attitude opaque"), doctest::Contains(given + ". A name of the engine's vocabulary cannot be given away."), std::runtime_error);
            CHECK(answers_of(collector, interactive, "P ~ opaque").empty());
            CHECK(answers_of(collector, interactive, "P ~ attitude") == std::vector<std::string>{"believes ~ attitude"});
        }
        SUBCASE("a predicate declared")
        {
            interactive.process("knows ~ opaque");
            const std::string taken = "Node " + std::to_string(interactive.graph()->get_node("opaque", "zelph")) + " carries the name 'opaque' of the engine's vocabulary in language 'zelph'. A name of the engine's vocabulary cannot be taken from its node.";

            SUBCASE("given to the class")
            {
                CHECK_THROWS_WITH_AS(interactive.process(".name attitude opaque"), doctest::Contains(given + ", where it names node"), std::runtime_error);
                CHECK(answers_of(collector, interactive, "P ~ attitude") == std::vector<std::string>{"believes ~ attitude"});
            }
            SUBCASE("the class merged into it under a name in another language")
            {
                process_lines(interactive, ".name attitude de haltung");
                CHECK_THROWS_WITH_AS(interactive.process(".name opaque de haltung"), doctest::Contains(merge), std::runtime_error);
                CHECK(answers_of(collector, interactive, "P ~ attitude") == std::vector<std::string>{"believes ~ attitude"});
            }
            SUBCASE("merged into a class without a name in zelph")
            {
                process_lines(interactive, ".name attitude de haltung\n.delname attitude\n.name opaque de undurchsichtig\n.lang de");
                CHECK_THROWS_WITH_AS(interactive.process(".name haltung undurchsichtig"), doctest::Contains(merge), std::runtime_error);
                interactive.process(".lang zelph");
            }
            SUBCASE("taken by a rename to the name of the class")
            {
                CHECK_THROWS_WITH_AS(interactive.process(".name opaque attitude"), doctest::Contains(taken), std::runtime_error);
                CHECK(answers_of(collector, interactive, "P ~ attitude") == std::vector<std::string>{"believes ~ attitude"});
            }
            SUBCASE("taken by .delname")
            {
                CHECK_THROWS_WITH_AS(interactive.process(".delname opaque"), doctest::Contains(taken), std::runtime_error);
            }

            CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"knows ~ opaque"});
            interactive.process("sue knows (bob runs nil)");
            CHECK(answers_of(collector, interactive, "X runs nil").empty());
        }

        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"}); });
}

TEST_CASE("opaque: a declaration that a firing writes through a variable predicate is in force at once")
{
    // `(X sets R O) => (X R O)` writes `believes ~ opaque` derived from
    // `believes sets ~ opaque`. After any predicate had been declared, a
    // consequence involving a variable predicate was constructed within the
    // scope that collects a mention's markings, whatever the predicate
    // turned out to be bound to, and a declaration written within that
    // scope does not count as one: it was neither checked nor recorded,
    // thus `believes` claimed the next belief and became opaque only at the
    // next .save/.load.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("says ~ opaque");
        interactive.process("(X sets R O) => (X R O)");

        SUBCASE("the predicate is opaque from the firing on")
        {
            interactive.process("believes sets ~ opaque");
            REQUIRE(answers_of(collector, interactive, "P ~ opaque").size() == 2);
            interactive.process("john believes (mary waits nil)");
            CHECK(answers_of(collector, interactive, "X waits nil").empty());
        }
        SUBCASE("the declaration is refused while a fact of the predicate holds a statement")
        {
            interactive.process("john believes (mary waits nil)");
            collector.clear();
            interactive.process("believes sets ~ opaque");
            CHECK(any_output_contains(collector, "a predicate cannot be declared opaque while one of its facts holds a statement -- believes: john believes (mary waits nil)"));
            CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"says ~ opaque"});
        } });
}

TEST_CASE("opaque: a refused cluster drop keeps the cluster")
{
    // During a front end's dry run, a predicate is declared inside a
    // cluster of its own; a belief written outside it afterwards holds a
    // statement, so dropping the cluster would remove the declaration and
    // is refused. The refusal must leave the cluster in its original state,
    // so that it can still be dropped once the belief is no longer present.
    // When something outside refers to a node belonging to the cluster, the
    // drop follows a cascading route, and that route had already given up
    // the cluster's record before the refusal: the cluster was removed from
    // the list, yet its nodes stayed in the graph, now without any cluster
    // to drop them.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process("x believes y");
        process_lines(interactive, ".cluster d\nbelieves ~ opaque\nzed ~ thing\n.cluster default");
        SUBCASE("nothing outside refers to the cluster") {}
        SUBCASE("something outside refers to the cluster")
        {
            interactive.process("ann likes zed");
        }
        interactive.process("joe believes (x y z)");

        const auto clusters = graph->list_clusters();
        REQUIRE(clusters.size() == 1);
        CHECK_THROWS_WITH_AS(interactive.process(".cluster-drop d"),
                             doctest::Contains("a predicate's opacity cannot be removed while one of its facts holds a statement -- believes: joe believes (x y z)"),
                             std::runtime_error);
        CHECK(graph->list_clusters() == clusters);
        CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"believes ~ opaque"});

        interactive.process(".prune-facts joe believes (x y z)");
        CHECK_NOTHROW(interactive.process(".cluster-drop d"));
        CHECK(graph->list_clusters().empty());
        CHECK(answers_of(collector, interactive, "P ~ opaque").empty());
        CHECK(answers_of(collector, interactive, "X ~ thing").empty()); });
}

TEST_CASE("opaque: the declaration survives .save/.load and travels with a predicate slice")
{
    // A front end declares a predicate once per graph; the declaration
    // constitutes a fact, thus a load retrieves it, and a slice of the
    // predicate carries it alongside the declaration of the predicate's
    // relation type. Absent it, the loaded `believes` claimed the next
    // belief.
    namespace fs    = std::filesystem;
    const auto full = fs::temp_directory_path() / "zelph_opaque_declaration_full.bin";
    const auto cut  = fs::temp_directory_path() / "zelph_opaque_declaration_slice.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("believes ~ opaque");
        interactive.process("john believes (mary waits nil)");
        interactive.process(".save \"" + full.string() + "\"");
        interactive.process(".save-predicates \"" + cut.string() + "\" believes");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    SUBCASE(".save/.load")
    {
        interactive.process(".load \"" + full.string() + "\"");
    }
    SUBCASE("a predicate slice")
    {
        interactive.process(".load \"" + cut.string() + "\"");
    }

    CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
    CHECK(answers_of(collector, interactive, "X waits nil").empty());
    interactive.process("sue believes (bob runs nil)");
    CHECK(answers_of(collector, interactive, "X runs nil").empty());

    fs::remove(full);
    fs::remove(cut);
}

TEST_CASE("opaque: a name merge that folds the declared predicate into another node keeps it declared")
{
    // `.name thinks believes` says that the two nodes are identical: the
    // declared `believes` is merged into the node previously named
    // `thinks`, and the declaration is re-created on the surviving node,
    // which now carries the name `believes`. The index of opaque predicates
    // still held the node that disappeared, so the next belief claimed what
    // it holds, until a .save/.load operation read the declarations again.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("thinks ~ verb");
        interactive.process(".name thinks believes");
        REQUIRE(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"believes ~ opaque"});

        interactive.process("john believes (mary waits nil)");
        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n"); });
}

TEST_CASE("opaque: negation levels read nothing inside an opaque argument")
{
    // A rule generating a belief concerning `glad` yields no `glad` fact,
    // hence a negated `glad` condition beside it does not make the rule
    // negate what it derives. Without the declaration, the levels read the
    // belief's argument as produced and refused to stratify the rule (i06
    // m11).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");

        SUBCASE("without the declaration the argument counts")
        {
            interactive.process("((X tells Y), ¬(Y glad X)) => (X believes (Y glad X))");
            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "Not stratifiable"));
        }
        SUBCASE("with it nothing inside is read")
        {
            interactive.process("believes ~ opaque");
            interactive.process("((X tells Y), ¬(Y glad X)) => (X believes (Y glad X))");
            interactive.process("((X says Y), ¬(Y sad X)) => (X believes ((Y sad X) => (Y sad X)))");
            collector.clear();
            interactive.process(".strata");
            CHECK_FALSE(any_output_contains(collector, "Not stratifiable"));
        } });
}

TEST_CASE("opaque: a firing that writes a mentioned rule writes no negation tag of the firing rule")
{
    // A pin of what round 3 already performs (i06 m11b): the condition
    // that is negated within the firing rule corresponds to the fact `((Y
    // sad X) ~ negation)`, and its instance must not leak into the
    // mentioned rule the firing writes, or else `X ~ negation` answers and
    // the mention carries a tag that nobody wrote. The declaration changes
    // how the firing marks its arguments, which is precisely where such a
    // leakage would reemerge.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        SUBCASE("without the declaration") {}
        SUBCASE("with the declaration")
        {
            interactive.process("believes ~ opaque");
        }
        interactive.process("(¬(Y sad X), (X says Y)) => (X believes ((Y sad X) => (Y sad X)))");
        interactive.process("john says mary");

        CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes ((mary sad john) => (mary sad john))"});
        CHECK(answers_of(collector, interactive, "X ~ negation").empty()); });
}

TEST_CASE("opaque: a query asserts what it creates in an opaque argument, as under a mentioned rule")
{
    // Replaces "opaque: a query over an opaque predicate leaves no claim",
    // which was introduced in the initial formulation of opaque predicates.
    // The built-in opaque predicate determines this: a query asserts what it
    // creates under a `=>` that it mentions outside every operator
    // (mark_query_operands, "S noted ((e p f) => (g q h))" asserts both
    // components), and the arguments of a declared predicate must be read
    // identically. A program that poses a question without writing runs it
    // within a cluster it drops. Before the existence of opaque predicates,
    // this held by construction; it pins that the mention the parser now
    // wraps the argument in marks nothing in a query.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("S believes (mary waits nil)");
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});

        interactive.process("S noted ((e p f) => (g q h))");
        CHECK(answers_of(collector, interactive, "X p Y") == std::vector<std::string>{"e p f"});

        SUBCASE("a variable inside the opaque argument makes the statement a query")
        {
            // A variable that a mention resolves is part of the statement,
            // in contrast to a variable of a rule that the mention holds,
            // thus the statement asks and asserts what it generated beside
            // the variable.
            interactive.process("john believes ((sue sleeps nil) causes (Y waits nil))");
            CHECK(answers_of(collector, interactive, "X sleeps nil") == std::vector<std::string>{"sue sleeps nil"});
        } });
}

// What a query asserts within an opaque argument includes a refutation
// marking, and the marking associated with a fact that the graph claims was
// never applied during the session, because a claimed fact cannot be refuted:
// `X waits nil` continued to respond with `bob waits nil`. A .save and a
// subsequent .load read the marking back as a refutation, thereby causing the
// answer to disappear (Zelph::rebuild_refuted_index).
TEST_CASE("opaque: a marking a query asserts in an opaque argument refutes a claimed fact neither before nor after a reload")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_opaque_query_marking.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "believes ~ opaque\nbob waits nil\nS believes ((bob waits nil) ~ refuted)\n");
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"bob waits nil"});
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"bob waits nil"}); });
    std::filesystem::remove(file);
}

// A query asserts the ground parts of a rule it refers to ("S noted ((e p f)
// => (g q h))" above), and a variable within the mentioned rule makes the
// rule part of the query's text. The condition set holds the variable,
// meaning no structure reached it: `S in O` answered `(e p f) in {(X p Y)
// (e p f)}` and `S ~ conjunction` answered the set's tag, in the session
// already. The text now goes with the answered query, and the ground
// condition stays asserted.
TEST_CASE("opaque: a query keeps the ground condition of a rule it mentions and none of the rule's text")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_mentioned_rule_query.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process("S noted ((X p Y, e p f) => (X q Y))");
        SUBCASE("in the session") {}
        SUBCASE("after .save and .load")
        {
            interactive.process(".save " + path);
            interactive.process(".load " + path);
        }

        CHECK(answers_of(collector, interactive, "X p Y") == std::vector<std::string>{"e p f"});
        CHECK(answers_of(collector, interactive, "S in O").empty());
        CHECK(answers_of(collector, interactive, "S ~ conjunction").empty()); });
    std::filesystem::remove(path);
}

TEST_CASE("opaque: a question asked in a cluster that is then dropped leaves nothing behind")
{
    // The reason the query semantics above costs a front end nothing: it
    // poses its questions within a scratch cluster and drops it, and the
    // drop takes away what the question asserted in the opaque argument,
    // along with the question itself. If a question's writes were ever
    // recorded outside the active cluster, the assertion would survive past
    // the question's lifetime.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        process_lines(interactive, ".cluster q\nX believes (mary waits nil)");
        REQUIRE(answers_of(collector, interactive, "X waits nil") == std::vector<std::string>{"mary waits nil"});

        process_lines(interactive, ".cluster default\n.cluster-drop q");
        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(answers_of(collector, interactive, "S believes O").empty());
        CHECK(answers_of(collector, interactive, "P ~ opaque") == std::vector<std::string>{"believes ~ opaque"}); });
}

TEST_CASE("mention: an island inside a mention is a mention")
{
    // An inline keyword island operates during the line's parsing phase,
    // meaning the content it constructs is stored in its dedicated scratch
    // before execution of the statement's code. A mention counts that
    // record as its own, just as a rule build and a query do: "John
    // believes that 1 + 1 = 2." otherwise left the island's facts claimed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/fact "a" "b" (string/trim t)))))js");
        interactive.process("believes ~ opaque");
        interactive.process("john believes $(c)");

        CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (a b c)"});
        CHECK(answers_of(collector, interactive, "X b c").empty()); });
}

TEST_CASE("opaque: the parser resolves the predicate without creating it")
{
    // While compiling a statement, the parser checks whether its predicate
    // has been declared opaque. Asked by creating the name, the question
    // left the predicate behind when the statement then failed before
    // naming it: in this case, the subject is refused before the predicate
    // is resolved. A pin of what holds without the check, against a check
    // that creates.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        CHECK_THROWS(interactive.process("(x in {a b}) unheard y"));
        CHECK(janet_value(collector, interactive, R"((zelph/lookup "unheard"))") == "nil"); });
}

TEST_CASE("opaque: pruning a claimed statement that a mention also holds keeps the mention")
{
    // "Mary waits." was asserted, afterwards "John believes that Mary
    // waits." reused the claimed node. Withdrawing the claim ought to
    // preserve the belief: the node stays as the mention the belief holds,
    // just as a rule's asserted pattern stays as the rule's pattern. Taken
    // away with its holders, the pruning process carried the belief along.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        interactive.process("mary waits nil");
        interactive.process("john believes (mary waits nil)");

        SUBCASE("the ground form")
        {
            interactive.process(".prune-facts mary waits nil");
        }
        SUBCASE("a pattern")
        {
            // A pattern containing a variable undergoes unification during
            // matching, and what it matches is handled in
            // Reasoning::prune_facts, the second path to the withdrawal.
            interactive.process(".prune-facts X waits nil");
        }

        CHECK(answers_of(collector, interactive, "X waits nil").empty());
        CHECK(answers_of(collector, interactive, "S believes O") == std::vector<std::string>{"john believes (mary waits nil)"});
        CHECK(explained(collector, interactive, "mary waits nil") == "mary waits nil [mentioned; not asserted]\n"); });
}

// `zelph/mention` writes a statement down without claiming it, as a rule
// writes its ground pattern, and a claimed statement that a rule's pattern
// or an opaque fact holds keeps its node during a prune (as shown above).
// Beside a claimed `a p b`, the mention `x says (a p b)` went with the claim
// instead, resulting in "Pruned 3" and the elimination of the mention,
// whereas `zelph/holders`, which leaves a mention out, had listed nothing:
// the prune took what the question had not named. The statement now stays
// as the mention's part, in each of the three forms of the prune.
TEST_CASE("opaque: pruning a claimed statement that zelph/mention also holds keeps the mention")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process(R"(%(def m (zelph/mention "x" "says" (zelph/fact "a" "p" "b"))))");
        REQUIRE(janet_value(collector, interactive, R"((zelph/holders "a" "p" "b"))") == "()");

        SUBCASE("the ground form")
        {
            collector.clear();
            interactive.process(".prune-facts a p b");
            CHECK(any_output_contains(collector, "Pruned 1 matching facts"));
        }
        SUBCASE("a pattern")
        {
            collector.clear();
            interactive.process(".prune-facts A p b");
            CHECK(any_output_contains(collector, "Pruned 1 matching facts"));
        }
        SUBCASE("zelph/prune-fact")
        {
            CHECK(janet_value(collector, interactive, R"((zelph/prune-fact "a" "p" "b"))") == ":unclaimed");
        }

        CHECK(answers_of(collector, interactive, "A p b").empty());
        CHECK(janet_value(collector, interactive, "(> (zelph/degree m) 0)") == "true");
        CHECK(explained(collector, interactive, "a p b") == "a p b [mentioned; not asserted]\n"); });
}

// A query writes what it builds beneath a condition operator as a pattern,
// and anything that holds none of its variables stays (zelph/query-text):
// following `S q O, ¬(a p b)` the negation `(a p b) ~ negation` stays marked
// beside the claimed `a p b`. That residue is not a mention. Counted as one,
// it made a later prune withdraw the claim of `a p b` rather than remove the
// fact: zelph/prune-fact answered :unclaimed, and `.explain` identified
// `a p b` as the pattern of a rule that does not exist. A question changes
// nothing that a prune performs, in any of the three forms of the prune.
TEST_CASE("opaque: what a query wrote beneath a condition operator holds no claimed statement on a prune")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto pruned = [&](const std::string& query, const std::string& fact, const std::string& pattern, const std::string& args)
        {
            process_lines(interactive, fact + "\nm q n\n" + query + "\n");
            SUBCASE("the ground form") { interactive.process(".prune-facts " + fact); }
            SUBCASE("a pattern") { interactive.process(".prune-facts " + pattern); }
            SUBCASE("zelph/prune-fact") { CHECK(janet_value(collector, interactive, "(zelph/prune-fact " + args + ")") == ":removed"); }
            CHECK(janet_value(collector, interactive, "(zelph/mentioned " + args + ")") == "false");
        };

        SUBCASE("a negation of the statement") { pruned("S q O, ¬(a p b)", "a p b", "A p b", R"("a" "p" "b")"); }
        SUBCASE("a negation of a statement that nests it") { pruned("S q O, ¬(x says (a p b))", "a p b", "A p b", R"("a" "p" "b")"); }
        SUBCASE("a ground path condition over it") { pruned("X P279 d, a P279⁺ d", "a P279 d", "A P279 d", R"("a" "P279" "d")"); } });
}
