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
#include "network/zelph.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// A rule's patterns are not data.
//
// Writing a rule materializes its conditions and its consequences as real
// fact nodes, because the engine has nothing else to match against. A pattern
// carrying a variable gives itself away as a template and is rejected as data
// everywhere. A GROUND one does not, and the consequences were severe and
// silent:
//
//     zelph> (a p b) => (c q d)
//     zelph> C q D
//     Answer: c q d          <- nobody said this
//
// It was not confined to queries -- the leaked pattern satisfied other rules'
// conditions -- and the shape that bites hardest is a ground CONSEQUENCE
// under a variable condition: "(A is bad) => (alarm is on)" answered "alarm
// is on" with nothing bad anywhere.
//
// The node itself cannot say which happened; asserting a statement and
// building it as a pattern produce the same node with the same edges. What
// can say it is the MOMENT of construction, and that is what the marking uses
// (see Zelph::mark_rule_patterns). Asserting or deriving the statement later
// revokes the mark, so the tests below come in pairs: the pattern is inert,
// and the moment it is claimed it behaves like any other fact.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // Whether no answer mentions `text`: answers_contain compares whole
    // answers.
    bool no_answer_mentions(const zelph::io::OutputCollector& collector, const std::string& text)
    {
        const std::vector<std::string> all = collect_answers(collector);
        return std::none_of(all.begin(), all.end(), [&](const std::string& a)
                            { return a.find(text) != std::string::npos; });
    }
}

TEST_CASE("rule patterns: a ground pattern answers no query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b")); });
}

TEST_CASE("rule patterns: a focus on a ground rule's part leaves it a pattern")
{
    // A rule statement whose focus causes it to evaluate to one of the rule's
    // parts claimed that part at the end of the statement, since a statement
    // claims what it evaluates to: `*(a p b) => (c q d)` asserted `a p b`, and
    // the rule it had just written derived `c q d`. The statement claims its
    // rule, just as the plain spelling does.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the condition")
        {
            interactive.process("*(a p b) => (c q d)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("A p B");
            CHECK_FALSE(answers_contain(collector, "a p b"));

            collector.clear();
            interactive.process("C q D");
            CHECK_FALSE(answers_contain(collector, "c q d"));
        }
        SUBCASE("the consequence")
        {
            interactive.process("(a p b) => *(c q d)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("C q D");
            CHECK_FALSE(answers_contain(collector, "c q d"));
        } });
}

TEST_CASE("rule patterns: a focus on a rule's arrow leaves its ground parts patterns")
{
    // The parser read a rule with a focus on its arrow as an ordinary
    // statement, which claims what it writes: `(a p b) *=> (c q d)` asserted
    // `a p b` and `c q d`, and even when a variable appeared in the
    // condition, the ground consequence was still asserted. The focus on the
    // arrow names the rule, and the line is the rule statement it spells
    // (`zelph-arrow`). Each subcase claims the condition afterwards, and the
    // rule fires.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a ground rule")
        {
            interactive.process("(a p b) *=> (c q d)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("A p B");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("C q D");
            CHECK(collect_answers(collector).empty());

            interactive.process("a p b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("C q D");
            CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"});
        }
        SUBCASE("a ground consequence under a condition with a variable")
        {
            interactive.process("(X is bad) *=> (alarm is on)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S is on");
            CHECK(collect_answers(collector).empty());

            interactive.process("smoke is bad");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S is on");
            CHECK(collect_answers(collector) == std::vector<std::string>{"alarm is on"});
        } });
}

TEST_CASE("rule patterns: a ground pattern satisfies no other rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("(X q Y) => (X r Y)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X r Y");
        CHECK_FALSE(answers_contain(collector, "c r d")); });
}

// The mark `(P) ~ "rule pattern"` indicates that the engine stores P as a
// pattern; it conveys no information about P itself. It answered `S ~ O`,
// and a rule operating on `~` facts fed on it: `(S ~ Y) => (S k Y)` derived
// `(a p b) k "rule pattern"`, while `(S ~ Y) => ((t p b) ~ Y)` wrote the
// mark for the claimed `t p b`. A load rebuilds the patterns' index from the
// marks, ensuring the round trip yields identical results, and the mark
// continues its function: upon a pattern being claimed, the rule fires.
TEST_CASE("rule patterns: the mark of a pattern answers no query and satisfies no rule")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_mark_unanswered.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        process_lines(interactive, "(a p b) => (c q d)\nt p b\n");
        SUBCASE("in the session") {}
        SUBCASE("after .save and .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            interactive.process(".auto-run"); // a load disables it
        }

        CHECK(answers(R"(S ~ "rule pattern")").empty());
        process_lines(interactive, "(S ~ Y) => (S k Y)\n(S ~ Y) => ((t p b) ~ Y)\n");
        CHECK(answers(R"(S k "rule pattern")").empty());
        CHECK(answers(R"(S ~ "rule pattern")").empty());
        CHECK(answers("S p O") == std::vector<std::string>{"t p b"});

        interactive.process("a p b");
        CHECK(answers("S q O") == std::vector<std::string>{"c q d"}); });
    fs::remove(file);
}

// The mark `(P) ~ "rule pattern"` is the engine's own bookkeeping, and upon
// reloading, the index of rules' patterns is reconstructed from each such
// fact. Written by hand, it functioned as a standard fact within the session
// that produced it: beside a claimed `a p b`, the line
// `(a p b) ~ "rule pattern"` had no effect until a .save followed by a .load,
// after which `a p b` answered no query, and so did the same mark nested
// within a statement, written by a program, derived by a rule, or standing in
// a rule's condition. Each of these routes is now blocked, yet the marks
// written by the engine itself remain functional, even following the load.
TEST_CASE("rule patterns: a mark written by hand is refused on every route, so a claimed statement stays one after a load")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_mark_by_hand.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto found = collect_answers(collector);
            std::sort(found.begin(), found.end());
            return found;
        };
        const auto refused = [&](const std::string& line)
        { CHECK_THROWS_WITH(interactive.process(line), doctest::Contains("only the engine writes it")); };

        process_lines(interactive, "a p b\n(c p d) => (e q f)\n");
        SUBCASE("on a line of its own") { refused(R"((a p b) ~ "rule pattern")"); }
        SUBCASE("nested in a statement") { refused(R"(x says ((a p b) ~ "rule pattern"))"); }
        SUBCASE("in a rule's condition") { refused(R"(((a p b) ~ "rule pattern") => (g r h))"); }
        SUBCASE("by a program") { refused(R"js(%(zelph/fact (zelph/fact "a" "p" "b") "~" (zelph/resolve "rule pattern" "zelph")))js"); }
        // A ground statement in a query's comma list is claimed with the
        // query, and a statement located beneath a negation stays as the
        // query's pattern, so either one wrote a mark that a subsequent load
        // interpreted as the engine's.
        SUBCASE("in a query's comma list") { refused(R"(X q Y, (a p b) ~ "rule pattern")"); }
        SUBCASE("under a negation in a query") { refused(R"(X q Y, ¬((a p b) ~ "rule pattern"))"); }
        // The auto-run initiated after the rule's line issues a refusal
        // report and records it: the explicit run below finds the record.
        SUBCASE("by a firing")
        {
            collector.clear();
            interactive.process(R"((X p Y) => ((X p Y) ~ "rule pattern"))");
            CHECK(any_event_contains(collector, "only the engine writes it"));
            CHECK_NOTHROW(interactive.run(true, false, false));
        }
        // One level down in the consequence, the act of firing builds the
        // mark as a part before the statement that holds it. The refusal
        // escaped the run at that point, and due to auto-run, the rule's
        // line and each subsequent line failed alongside it; it is reported
        // exactly as the one above is.
        SUBCASE("by a firing that nests it")
        {
            collector.clear();
            CHECK_NOTHROW(interactive.process(R"((X p Y) => (z says ((X p Y) ~ "rule pattern")))"));
            CHECK(any_event_contains(collector, "only the engine writes it"));
            CHECK_NOTHROW(interactive.run(true, false, false));
        }

        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(answers("S p O") == std::vector<std::string>{"a p b"});

        interactive.process("c p d");
        interactive.run(true, false, false);
        CHECK(answers("S p O") == std::vector<std::string>{"a p b", "c p d"});
        CHECK(answers("S q O") == std::vector<std::string>{"e q f"}); });
    fs::remove(file);
}

// A firing builds the statement it marks before the mark. In the case of a
// statement that no one had written, the refusal above left `a q b` as data
// that no derivation line named, and the rule over `q` fired on it during the
// same run. A refused consequence leaves the graph unchanged.
TEST_CASE("rule patterns: a firing refused for writing the mark leaves no statement it built")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\n(X q Y) => (X ok Y)\n");
        collector.clear();
        CHECK_NOTHROW(interactive.process(R"((X p Y) => ((X q Y) ~ "rule pattern"))"));
        CHECK(any_event_contains(collector, "only the engine writes it"));
        for (const char* query : {"S q O", "S ok O"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

// A command that names a statement constructs it within a scratch cluster and
// discards it before reading the answer (.prune-facts, .prune-nodes,
// .explain), meaning the mark created for a statement that is no rule's
// pattern is written solely for that question and never persists. Rejected
// akin to a mark written by hand,
// `.prune-facts ((a p b) ~ "rule pattern")` returned the error related to
// writing the mark, while a prune of any other absent statement answers
// "Pruned 0", and `.explain` reported a pattern it could not parse.
TEST_CASE("rule patterns: a command naming a mark that no rule made answers as for any absent statement")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_mark_named_by_command.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto found = collect_answers(collector);
            std::sort(found.begin(), found.end());
            return found;
        };
        process_lines(interactive, "a p b\n(c p d) => (e q f)\n");
        const std::string mark = R"(((a p b) ~ "rule pattern"))";

        collector.clear();
        CHECK_NOTHROW(interactive.process(".prune-facts " + mark));
        CHECK(any_output_contains(collector, "Pruned 0 matching facts."));
        collector.clear();
        CHECK_NOTHROW(interactive.process(".prune-nodes " + mark));
        CHECK(any_output_contains(collector, "Pruned 0 matching facts and 0 nodes"));
        collector.clear();
        CHECK_NOTHROW(interactive.process(".explain " + mark));
        CHECK(any_output_contains(collector, "not asserted"));
        CHECK(answers("S p O") == std::vector<std::string>{"a p b"});

        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(answers("S p O") == std::vector<std::string>{"a p b"}); });
    fs::remove(file);
}

TEST_CASE("rule patterns: the shape that bites -- a ground consequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is bad) => (alarm is on)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X is on");
        REQUIRE_FALSE(answers_contain(collector, "alarm is on"));

        // ... and the moment something IS bad, it is an ordinary fact.
        interactive.process("rust is bad");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X is on");
        CHECK(answers_contain(collector, "alarm is on")); });
}

TEST_CASE("rule patterns: being derived announces the statement")
{
    // Revoking the mark is the moment the statement BECOMES data, and
    // everything that reacts to a new fact has to hear about it. Without the
    // announcement, semi-naive seeding never offers it to the rules whose
    // conditions it now satisfies -- the node itself is old, so nothing else
    // would mention it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(A is bad) => (alarm is on)");
        interactive.process("(X is on) => (X needs attention)");
        collector.clear();
        interactive.process("rust is bad");
        CHECK(any_deduction_of(collector, "alarm is on"));
        CHECK(any_deduction_of(collector, "alarm needs attention"));

        collector.clear();
        interactive.process("S needs O");
        CHECK(answers_contain(collector, "alarm needs attention")); });
}

TEST_CASE("rule patterns: a derived statement claims the facts nested in it")
{
    // A derivation is a claim and reaches as far as a typed statement: the
    // facts nested in what a firing derives are claimed, outside a `=>`.
    // Previously, only the derived node was claimed, so a part that a rule
    // had written remained a pattern, whereas the identical statement, once
    // typed, made it data. Each claimed part is announced, and the rule over
    // it fires.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        SUBCASE("a ground part of the consequence")
        {
            interactive.process("(X go Y) => ((a p b) q Y)");
            interactive.process("t go z");
        }
        SUBCASE("a ground part nested deeper")
        {
            // The firing builds `(a p b) q z` and claims it, with the claim
            // continuing into what the fact holds.
            interactive.process("(X go Y) => (k says ((a p b) q Y))");
            interactive.process("t go z");
        }
        SUBCASE("a ground part that the binding supplies as well")
        {
            // The rule over rules binds G to `a p b`, and its consequence
            // writes the identical fact as it stands: what the consequence
            // writes is claimed, regardless of whether a variable represents
            // it in another context.
            interactive.process("(G => H) => ((a p b) seen G)");
        }
        // A firing keeps the comma list of its consequence exactly as written,
        // and its claim did not enter it: `a p b` stayed a pattern, whereas the
        // typed `k says (a p b, j r s)` and the set form made it data.
        SUBCASE("a member of a comma list the consequence writes")
        {
            interactive.process("(X go Y) => (k says (a p b, j r s))");
            interactive.process("t go z");
        }
        // A comma list among the members is entered in turn, just as in the
        // typed statement.
        SUBCASE("a member of a comma list among the members of one")
        {
            interactive.process("(X go Y) => (k says ((a p b, j r s), m n o))");
            interactive.process("t go z");
        }
        // The binding supplies `a p b` as G, and the comma list writes the
        // same fact exactly as it stands, which the claim makes data, as
        // above.
        SUBCASE("a member of a comma list that the binding supplies as well")
        {
            interactive.process("(G => H) => (k seen (a p b, j r s))");
        }
        // The claim reads the subject of the consequence in the same
        // manner as it reads the objects, and a comma list there is also
        // entered.
        SUBCASE("a member of a comma list the consequence writes as its subject")
        {
            interactive.process("(X go Y) => ((a p b, j r s) says Y)");
            interactive.process("t go z");
        }
        // A comma list beneath a statement that the consequence writes is
        // entered once the claim has entered that statement, just as in the
        // typed statement.
        SUBCASE("a member of a comma list nested in a statement the consequence writes")
        {
            interactive.process("(X go Y) => (k says (m says (a p b, j r s)))");
            interactive.process("t go z");
        }
        // The firing builds the set for its binding and keeps the comma list
        // within it as written; the claim enters the one, and subsequently
        // the other.
        SUBCASE("a member of a comma list in a set the firing builds")
        {
            interactive.process("(X go Y) => (k says {(X q r) (a p b, j r s)})");
            interactive.process("t go z");
        }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

// The boundary of the comma-list SUBCASEs above, akin to a typed statement: a
// firing claims the members of a comma list its consequence writes, not of
// one its text holds by node. A rule's condition set constitutes such a comma
// list, and upon being entered, it would turn that rule's conditions into
// data, and the rule would fire upon them.
TEST_CASE("rule patterns: a comma list a consequence names by node is not entered")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b, c d e) => (f g h)");
        interactive.process(R"js(%(def conditions (get (zelph/parse (get (first (zelph/rules)) :rule)) 0)))js");
        SUBCASE("as an object") { interactive.process("(X go Y) => (k says ,conditions)"); }
        SUBCASE("as a member of a comma list the consequence writes") { interactive.process("(X go Y) => (k says (,conditions, j r s))"); }
        interactive.process("t go z");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).size() == 1);
        for (const std::string query : {"S p O", "S d O", "S g O"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

// The other boundary of the comma-list SUBCASEs above: an argument of an
// opaque predicate constitutes a mention, and a comma list it holds is not
// entered, neither as the argument nor beneath it, just as a typed statement
// leaves it untouched. If entered, it would make `a p b` data, and
// `(a p b) => (c q d)` would fire.
TEST_CASE("rule patterns: a comma list an opaque argument of a consequence holds is not entered")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "believes ~ opaque\n(a p b) => (c q d)\n");
        SUBCASE("as the argument") { interactive.process("(X go Y) => (k believes (a p b, j r s))"); }
        SUBCASE("in an argument") { interactive.process("(X go Y) => (k believes ((a p b, j r s), m n o))"); }
        SUBCASE("as the argument of a nested statement") { interactive.process("(X go Y) => (k says (m believes (a p b, j r s)))"); }
        interactive.process("t go z");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S believes O");
        CHECK(collect_answers(collector).size() == 1);
        for (const std::string query : {"S p O", "S r O", "S q O"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

TEST_CASE("rule patterns: a rule over rules asserts nothing of the rule it reads")
{
    // A derivation claims what its consequence writes, not the values its
    // binding supplies: the firing read those. A rule over rules binds the
    // parts of the rules it reads, so `(G => H) => (G seen H)` writes
    // `(rust is bad) seen (alarm is on)` and asserts neither part, and a rule
    // that is only mentioned stays a rule nobody put in force. When it
    // claimed them, any such rule over rules turned the condition and the
    // consequence of every ground rule it read into data, and for a rule with
    // two conditions the consequence alone. The same statement typed by the
    // user writes both parts and claims both.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        const auto explained = [&](const std::string& fact)
        {
            collector.clear();
            interactive.process(".explain " + fact);
            return collector;
        };
        process_lines(interactive, R"(
((X is bad) => (alarm is on)) "was proposed by" alice
alice says ((rust is bad) => (alarm is on))
)");

        SUBCASE("a mentioned rule that a rule over rules reads")
        {
            interactive.process("(G => H) => (G seen H)");
            interactive.run(true, false, false);
            CHECK(answered("S seen O") == std::vector<std::string>{"(rust is bad) seen (alarm is on)"});
            CHECK(answered("A is B").empty());
            CHECK(any_output_contains(explained("(alarm is on)"), "alarm is on  [rule pattern; not asserted]"));
        }
        SUBCASE("a mentioned rule that a rule over rules reads, beside a fact its consequence writes")
        {
            // The consequence writes `k is listed` and the fact around the
            // two values, and the claim walks through both; the values it
            // meets inside remain the ones the binding supplied.
            interactive.process("(G => H) => ((k is listed) about (G seen H))");
            interactive.run(true, false, false);
            CHECK(answered("A is B") == std::vector<std::string>{"k is listed"});
            CHECK(any_output_contains(explained("(alarm is on)"), "alarm is on  [rule pattern; not asserted]"));
        }
        SUBCASE("the statement it derives, typed")
        {
            interactive.process("(rust is bad) seen (alarm is on)");
            interactive.run(true, false, false);
            CHECK(answered("A is B") == std::vector<std::string>{"alarm is on", "rust is bad"});
            CHECK(any_output_contains(explained("(alarm is on)"), "alarm is on  [axiom]"));
        } });
}

// A rule that zelph/mention writes, and one that an opaque argument holds,
// bears the mark of a mention, causing unification to skip a marked
// candidate: a rule over rules does not read it, just as no query and no
// other condition takes a mention as data (.help opaque). Unmarked, both
// were read and written into `seen`. A rule that a plain statement holds is
// still read, as alice's proposal is in the case above.
TEST_CASE("rule patterns: a rule over rules does not read a rule written as a mention")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
believes ~ opaque
alice says ((rust is bad) => (alarm is on))
john believes ((tea is cold) => (kettle is on))
%(zelph/mention (fn [] (zelph/fact (zelph/fact "door" "is" "open") "=>" (zelph/fact "draft" "is" "on"))))
(G => H) => (G seen H)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(rust is bad) seen (alarm is on)"}); });
}

// A rule over rules reads the conditions of any arity via the memberships
// of the rule's condition set, of a rule currently in force and of one a
// statement holds alike: the memberships of a rule's condition set constitute
// its text, and that is data. Queries and conditions bypassed them
// temporarily, and both rules below derived nothing.
TEST_CASE("rule patterns: a rule over rules reads the conditions of a rule through the memberships of its condition set")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_reads_conditions.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };

        const auto written = [&](const std::string& statement)
        {
            interactive.process(statement);
            SUBCASE("in the session") {}
            SUBCASE("after .save and .load")
            {
                interactive.process(".save \"" + file.string() + "\"");
                interactive.process(".load \"" + file.string() + "\"");
                interactive.process(".auto-run"); // a load disables it
            }
        };

        bool held = false;
        SUBCASE("a rule a statement holds")
        {
            written("alice says ((a p b, c q d) => (alarm is on))");
            held = true;
        }
        SUBCASE("a rule in force") { written("(a p b, c q d) => (alarm is on)"); }

        process_lines(interactive, "(G => H, M in G) => (M cond-of H)\n(Z says (G => H), M in G) => (M said-cond Z)\n");
        CHECK(answered("S cond-of O") == std::vector<std::string>{"(a p b) cond-of (alarm is on)", "(c q d) cond-of (alarm is on)"});
        CHECK(answered("S said-cond O") == (held ? std::vector<std::string>{"(a p b) said-cond alice", "(c q d) said-cond alice"} : std::vector<std::string>{}));

        // The rule over rules asserts none of the conditions it examines.
        CHECK(answered("S p O").empty());
        CHECK(answered("S q O").empty()); });
    fs::remove(file);
}

// The text of a rule constitutes data during a seeded run too: a rule
// governing rules meets the tag and the memberships of a condition set
// written since the last run as new facts, for a rule a statement holds
// and for one in force. A change that keeps rule text out of the record of
// additions, as the accounting of removals already does for the tags,
// would result in `.run-delta` without either derivation.
TEST_CASE("rule patterns: a seeded run offers the text of a rule written since the last run to a rule over rules")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        const auto seeded = [&]
        {
            interactive.process(".auto-run");
            process_lines(interactive, "(G => H, M in G) => (M cond-of H)\n(Z says (G => H), M in G) => (M said-cond Z)\n.run\n");

            bool held = false;
            SUBCASE("a rule a statement holds")
            {
                interactive.process("alice says ((a p b, c q d) => (alarm is on))");
                held = true;
            }
            SUBCASE("a rule in force") { interactive.process("(a p b, c q d) => (alarm is on)"); }

            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            CHECK(answered("S cond-of O") == std::vector<std::string>{"(a p b) cond-of (alarm is on)", "(c q d) cond-of (alarm is on)"});
            CHECK(answered("S said-cond O") == (held ? std::vector<std::string>{"(a p b) said-cond alice", "(c q d) said-cond alice"} : std::vector<std::string>{}));
        };

        SUBCASE("check mode") { seeded(); }
        SUBCASE(".semi-naive on")
        {
            interactive.process(".semi-naive on");
            seeded();
        } });
}

TEST_CASE("rule patterns: a rule over rules asserts no part of a typed rule it reads")
{
    // G binds the condition, or the condition set, and H the consequence;
    // the derived statement claims none of these, and the typed rule stays
    // silent. A derivation that claimed the values it bound made both parts
    // of a rule with a single condition data, thus triggering the rule, and
    // for a rule with several conditions, only the consequence, which then
    // answered while the conditions it depends on remained patterns.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("one condition")
        {
            interactive.process("(a p b) => (e r f)");
        }
        SUBCASE("several conditions")
        {
            interactive.process("(a p b, c q d) => (e r f)");
        }
        interactive.process("(G => H) => (G seen H)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector).size() == 1);

        for (const std::string query : {"S p O", "S q O", "S r O"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        }

        collector.clear();
        interactive.process(".explain (e r f)");
        CHECK(any_output_contains(collector, "e r f  [rule pattern; not asserted]")); });
}

TEST_CASE("rule patterns: a derivation's claim reads no term that earlier firings filled")
{
    // A term into which each firing writes a member is not entered by the
    // claim of a firing: it retains the content all prior firings
    // contributed, and entering it caused the cost of a run to increase
    // with the square of the number of firings (4000 firings: 0.01 s
    // escalated to 3 s). Claimed is what the firing wrote; a term is
    // entered solely when the firing built it under its binding, at a
    // value position. Zelph::claim_walk_steps counts the statements and
    // nodes the claims take up and the memberships they read, independently
    // of the machine; the facts are written with evaluation disabled,
    // ensuring only the firings are counted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        constexpr int firings = 500;

        SUBCASE("a term the consequence names at a value position")
        {
            // The rule over rules binds C to the typed rule's collection,
            // and the rule it produces names the collection's term in its
            // place, at both positions. A firing of the generated rule did
            // not build that term.
            process_lines(interactive, R"(
.deductions off
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => ((X in C) noted C))
)");
        }
        SUBCASE("a term the consequence writes into")
        {
            interactive.process(".deductions off");
            interactive.process("(a p b) => (c q d)");
            interactive.process("(X r Y) => ((X in @{z}) noted X)");
        }
        interactive.run(true, false, false);
        interactive.process(".auto-run"); // a toggle: off
        interactive.process("%(for i 0 " + std::to_string(firings) + R"( (let [k (string "k" i)] (zelph/fact k "r" k))))");

        const uint64_t steps = zelph::network::Zelph::claim_walk_steps();
        interactive.run(true, false, false);
        CHECK(zelph::network::Zelph::claim_walk_steps() - steps <= 16 * firings);

        collector.clear();
        interactive.process("S noted O");
        CHECK(collect_answers(collector).size() == firings); });
}

TEST_CASE("rule patterns: a statement with nothing to claim costs the claim at most one step")
{
    // After a rule acquires a ground pattern, each statement can include a
    // part that carries a mark. A statement whose immediate parts provide
    // the walk with nothing to process is settled before the walk allocates
    // any resources, and two callers decide it without invoking a claim: a
    // fact originating from Janet whose parts bear no mark (those parts are
    // at hand) and a derivation whose consequence's parts are all values
    // supplied by its binding (read once per rule and run) take no step at
    // all, and a typed statement whose parts bear no mark costs the one step
    // associated with the statement. Each of these had allocated a walk per
    // statement, which cost a bulk Janet import after a single ground rule
    // 12 %.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        SUBCASE("a fact from Janet")
        {
            const uint64_t steps = zelph::network::Zelph::claim_walk_steps();
            interactive.process(R"(%(for i 0 500 (zelph/fact (zelph/fact (string "x" i) "r" "y") "s" "z")))");
            CHECK(zelph::network::Zelph::claim_walk_steps() == steps);
            collector.clear();
            interactive.process("S s O");
            CHECK(collect_answers(collector).size() == 500);
        }
        SUBCASE("a typed statement")
        {
            interactive.process(".auto-run"); // a toggle: off
            const uint64_t steps = zelph::network::Zelph::claim_walk_steps();
            for (int i = 0; i < 500; ++i)
                interactive.process("x" + std::to_string(i) + " s z");
            CHECK(zelph::network::Zelph::claim_walk_steps() - steps == 500);
            collector.clear();
            interactive.process("S s O");
            CHECK(collect_answers(collector).size() == 500);
        }
        SUBCASE("a derivation")
        {
            interactive.process("(X r Y) => (Y s X)");
            interactive.run(true, false, false);
            interactive.process(".auto-run"); // a toggle: off
            interactive.process(R"(%(for i 0 500 (let [k (string "k" i)] (zelph/fact k "r" "m"))))");
            const uint64_t steps = zelph::network::Zelph::claim_walk_steps();
            interactive.run(true, false, false);
            CHECK(zelph::network::Zelph::claim_walk_steps() == steps);
            collector.clear();
            interactive.process("S s O");
            CHECK(collect_answers(collector).size() == 500);
        }

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: asserting the statement first keeps it data")
{
    // Order matters and must not: a statement that was CLAIMED before any
    // rule mentioned it is a claim, and the rule reusing its node changes
    // nothing about that. The construction only ever marks what it created.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("(a p b) => (c q d)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        // The rule fires on it, so the consequence is a claim too now.
        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: asserting it afterwards revokes the mark")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: a ground comma list claims each of its members")
{
    // A comma list that contains no variables asserts every one of its
    // members, just as the line of each member does, and thus claims each,
    // with what it nests. The claim originated at the conjunction set, which
    // lacks any fact structure, and did not reach any member: after
    // `(a p b) => (c q d)`, the line `a p b, x q y` left `a p b` as a
    // pattern, and the rule was never fired.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        SUBCASE("a member") { interactive.process("a p b, x q y"); }
        SUBCASE("a fact nested in a member") { interactive.process("(a p b) q z, x q y"); }
        // A member that constitutes a comma list, which asserts its members
        // as its own line does, possesses a claim that originated at a
        // conjunction set too and failed to attain any of them.
        SUBCASE("a member of a comma list among the members") { interactive.process("(a p b, j r s), x q y"); }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: a comma list a statement names by node is not entered")
{
    // The boundary of the case above: a statement claims the comma lists it
    // generates, rather than those it holds by node. A rule's condition set
    // also constitutes a comma list, and once entered, it would turn the
    // rule's conditions into data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b, c d e) => (f g h)");
        interactive.process(R"js(%(def conditions (get (zelph/parse (get (first (zelph/rules)) :rule)) 0)))js");
        SUBCASE("as a member of a ground comma list") { interactive.process(",conditions, x q y"); }
        SUBCASE("as an object") { interactive.process("x says ,conditions"); }
        interactive.run(true, false, false);

        for (const std::string query : {"S p O", "S d O", "S g O"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

TEST_CASE("rule patterns: a statement claims the facts nested in it")
{
    // A statement claims itself and every fact nested in it outside a `=>`.
    // Previously, only its own node was considered claimed, meaning a nested
    // fact served as data when the statement created it and as a pattern
    // when a rule had created it first: following `(a p b) => (c q d)`, the
    // statement `(a p b) q z` left `a p b` unanswered and the rule silent,
    // whereas in reverse order both were answered. Each subcase puts the
    // part in a different position the claim reaches, and the rule fires
    // once the part becomes data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        SUBCASE("in the subject")
        {
            interactive.process("(a p b) q z");
        }
        SUBCASE("in an object")
        {
            interactive.process("z q (a p b)");
        }
        SUBCASE("nested deeper")
        {
            interactive.process("x says ((a p b) q z)");
        }
        SUBCASE("in a set constant")
        {
            interactive.process("x has {(a p b) k}");
        }
        SUBCASE("in a collection the statement writes")
        {
            interactive.process("x has @{(a p b) k}");
        }
        SUBCASE("in a collection the statement writes into")
        {
            // A collection written as a literal is the statement's own,
            // regardless of its position.
            interactive.process("(m n o) in @{(a p b)}");
        }
        SUBCASE("in a comma list the statement writes")
        {
            // A comma list that the statement writes is also its own, and
            // the claim did not enter it: `x says (a p b, j r s)` left
            // `a p b` a pattern, whereas the identical line input before the
            // rule made it data.
            interactive.process("x says (a p b, j r s)");
        }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "a p b  [axiom]")); });
}

TEST_CASE("rule patterns: a fact a focus writes beside a rule claims the facts nested in it")
{
    // A focus within a rule's slot creates a fact alongside the rule and
    // hands the part it marks to the rule (concepts.md, "The Focus Operator
    // `*`"). This fact constitutes a claim, and that claim claimed nothing
    // beyond itself: `(x says *(a p b)) => (c q d)` answered `x says (a p b)`
    // and left `a p b` the rule's pattern, so the rule never fired, whereas
    // the identical text typed as two lines -- the rule followed by
    // `x says (a p b)` -- claimed `a p b` and fired the rule. Each subcase
    // verifies that the parts the rule's own text holds stay patterns.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        SUBCASE("in the condition")
        {
            interactive.process("(x says *(a p b)) => (c q d)");
            interactive.run(true, false, false);
            CHECK(answers("X says Y") == std::vector<std::string>{"x says (a p b)"});
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("C q D") == std::vector<std::string>{"c q d"});
        }
        SUBCASE("in the consequence")
        {
            interactive.process("(k is on) => (x says *(c q d))");
            interactive.run(true, false, false);
            CHECK(answers("C q D") == std::vector<std::string>{"c q d"});
            CHECK(answers("K is O").empty());
        }
        // The rule's text reaches a collection created by the build via the
        // collection's memberships and tags, and the walk took every new fact
        // that holds the collection, or a new name, for that text. A fact
        // beside the rule that holds a new name or a new set constant, which
        // the rule also holds, was therefore part of the rule:
        // `(x says *(a p b)) => (x q d)` left `a p b` the rule's pattern, and
        // the rule never fired, whereas the two lines fired it.
        SUBCASE("beside a rule whose consequence holds its new subject")
        {
            interactive.process("(x says *(a p b)) => (x q d)");
            interactive.run(true, false, false);
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("X q D") == std::vector<std::string>{"x q d"});
        }
        SUBCASE("beside a rule whose consequence holds the new name it tags")
        {
            interactive.process("(x ~ *(a p b)) => (x q d)");
            interactive.run(true, false, false);
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("X q D") == std::vector<std::string>{"x q d"});
        }
        SUBCASE("beside a rule whose consequence holds its new set constant")
        {
            interactive.process("(x says *(a p b) {k}) => (c q {k})");
            interactive.run(true, false, false);
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("C q K") == std::vector<std::string>{"c q {k}"});
        }
        // The condition set holds its members via memberships, which are
        // the rule's text, just as the set itself is.
        SUBCASE("beside a ground member of the comma list")
        {
            interactive.process("(x says *(a p b), e f g) => (c q d)");
            interactive.run(true, false, false);
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("E f G").empty());
            CHECK(answers("C q D").empty());

            interactive.process("e f g");
            interactive.run(true, false, false);
            CHECK(answers("C q D") == std::vector<std::string>{"c q d"});
        }
        // A fact that holds the rule's variable is the rule's text, which no
        // query answers, and it claims nothing.
        SUBCASE("a fact beside the rule that holds a variable")
        {
            interactive.process("(x says *(X p (a r b))) => (X q b)");
            interactive.run(true, false, false);
            CHECK(answers("A r B").empty());
        }

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1")); });
}

TEST_CASE("rule patterns: a fact a focus writes is claimed when a rule wrote it as a pattern before")
{
    // A focus writes a fact beside the part it hands on, and the
    // statement claims it. Only a NEW fact was ever a claim, since nothing
    // marked it: one that a rule had already written as its pattern stayed
    // one. Following "(m n o) => (x says (a p b))", the rule
    // "(x says *(a p b)) => (c q d)" never fired, and "x says *(a p b)"
    // claimed `a p b` while `.explain` continued to treat `x says (a p b)`
    // as a pattern that is not asserted. The two-line spellings claim both.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        SUBCASE("beside a rule")
        {
            process_lines(interactive, "(m n o) => (x says (a p b))\n(x says *(a p b)) => (c q d)");
            interactive.run(true, false, false);
            CHECK(answers("X says Y") == std::vector<std::string>{"x says (a p b)"});
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("C q D") == std::vector<std::string>{"c q d"});
        }
        SUBCASE("beside the rule whose consequence it is")
        {
            interactive.process("(x says *(a p b)) => (x says (a p b))");
            interactive.run(true, false, false);
            CHECK(answers("X says Y") == std::vector<std::string>{"x says (a p b)"});
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
        }
        SUBCASE("on a line of its own")
        {
            process_lines(interactive, "(m n o) => (x says (a p b))\nx says *(a p b)");
            CHECK(answers("X says Y") == std::vector<std::string>{"x says (a p b)"});
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});

            collector.clear();
            interactive.process(".explain (x says (a p b))");
            CHECK_FALSE(any_output_contains(collector, "not asserted"));
        }
        SUBCASE("on a line that evaluates to the predicate")
        {
            process_lines(interactive, "(m n o) => (x says y)\nx *says y");
            CHECK(answers("X says Y") == std::vector<std::string>{"x says y"});
        } });
}

// A fact a focus writes is claimed as its line would claim it, and along
// with it the facts it holds. Adjacent to a marking that refutes the
// focused part, that claim incorporates the refuted fact, and the line is
// refused, as the two lines "x says (a p b)" and "(a p b) ~ refuted" are
// in either sequence. When `a p b` served as a rule's pattern, the line
// formerly was accepted: the fact next to it was not claimed, and `a p b`
// stayed a pattern of the rule even after being refuted. The refused line
// leaves no trace and prints nothing; its echo formerly preceded the
// refusal.
TEST_CASE("rule patterns: a fact a focus writes beside the part a line refutes is refused with the line")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A refutation creates the engine's name `refuted`, which is
        // assigned to no cluster and persists beyond a refused line; one
        // created beforehand keeps the node count exact.
        process_lines(interactive, "(a p b) => (c q d)\n¬(m n o)");
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        CHECK_THROWS_WITH(interactive.process("(x says *(a p b)) ~ refuted"), doctest::Contains("known to be wrong"));
        CHECK_FALSE(any_output_contains(collector, "refuted"));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("X says Y");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("X ~ refuted");
        const std::vector<std::string> refuted = collect_answers(collector);
        REQUIRE(refuted.size() == 1);
        CHECK(refuted[0].find("m n o") != std::string::npos);

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]")); });
}

// The identical line where the focused part is not a rule pattern. The
// marking was composed using zelph/fact around the part the focus handed
// over, which no refusal detected: following "a p b", the line "(x says
// *(a p b)) ~ refuted" refuted the claimed `a p b`, and on a graph lacking
// it, the line refuted `a p b` while asserting `x says (a p b)`, which
// holds it. Both two-line spellings are refused, in either order, and
// likewise the current line, leaving nothing: a claimed part is "known to
// be true" to the refutation, and a new one is "known to be wrong" to the
// claim adjacent to it. A marking embedded within a claimed statement and
// a member of a ground comma list are constructed identically.
TEST_CASE("rule patterns: a marking of the part a focus hands on is refused as its two lines are")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The engine's name `refuted` was established beforehand, just as in
        // the preceding case.
        interactive.process("¬(m n o)");

        std::string line;
        std::string refusal;
        bool        claimed = false;
        SUBCASE("the part claimed before")
        {
            line    = "(x says *(a p b)) ~ refuted";
            refusal = "known to be true";
            claimed = true;
        }
        SUBCASE("the part new")
        {
            line    = "(x says *(a p b)) ~ refuted";
            refusal = "known to be wrong";
        }
        SUBCASE("in a statement that holds the marking")
        {
            line    = "z says ((x says *(a p b)) ~ refuted)";
            refusal = "known to be true";
            claimed = true;
        }
        SUBCASE("in a ground comma list")
        {
            line    = "a p b, (x says *(a p b)) ~ refuted";
            refusal = "known to be true";
        }
        // The part that a focus hands on may emerge from a focus in turn:
        // `y says *(a p b)` evaluates to `a p b`, just as `*(a p b)` does.
        // The marking of this part was constructed using zelph/fact once
        // more, and following "a p b" it refuted the claimed `a p b`.
        SUBCASE("through a second focus: the part claimed before")
        {
            line    = "(x says *(y says *(a p b))) ~ refuted";
            refusal = "known to be true";
            claimed = true;
        }
        SUBCASE("through a second focus: the part new")
        {
            line    = "(x says *(y says *(a p b))) ~ refuted";
            refusal = "known to be wrong";
        }
        SUBCASE("through a focus on the part itself")
        {
            line    = "(x says *(*(a p b))) ~ refuted";
            refusal = "known to be true";
            claimed = true;
        }
        // When a focus lies on the whole subject, the line evaluates to
        // whatever the subject evaluates to, specifically the part the
        // inner focus marks.
        SUBCASE("through a focus on the whole subject")
        {
            line    = "*(x says *(a p b)) ~ refuted";
            refusal = "known to be true";
            claimed = true;
        }
        if (claimed) interactive.process("a p b");
        const std::size_t before = node_count(collector, interactive);

        collector.clear();
        CHECK_THROWS_WITH(interactive.process(line), doctest::Contains(refusal));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("X ~ refuted");
        const std::vector<std::string> refuted = collect_answers(collector);
        REQUIRE(refuted.size() == 1);
        CHECK(refuted[0].find("m n o") != std::string::npos);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == (claimed ? std::vector<std::string>{"a p b"} : std::vector<std::string>{})); });
}

// On the opposing side of the refusal: within an argument involving an
// opaque predicate, the fact beside the marking mentions the part rather
// than asserting it, and the line refutes the part, just as "john believes
// (e f g)" and "(e f g) ~ refuted" do when typed as two lines.
TEST_CASE("rule patterns: a marking of the part a focus hands on out of an opaque argument refutes it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("believes ~ opaque");
        CHECK_NOTHROW(interactive.process("(john believes *(e f g)) ~ refuted"));

        collector.clear();
        interactive.process(".explain (e f g)");
        CHECK(any_output_contains(collector, "¬(e f g)  [axiom]"));

        collector.clear();
        interactive.process("S believes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john believes (e f g)"}); });
}

// `*{...} ~ conjunction` is the comma list written out: the fact that its
// focus writes serves as the tag making the set the rule's condition list, a
// part of the rule's text, and a claim of it would claim the conditions
// alongside it. When typed with ground conditions, the rule fires precisely
// when those conditions are asserted, not before that.
TEST_CASE("rule patterns: the comma list written out claims none of its conditions")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(*{(a p b) (c q d)} ~ conjunction) => (e f g)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("E f G");
        CHECK(collect_answers(collector).empty());

        process_lines(interactive, "a p b\nc q d");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("E f G");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e f g"}); });
}

TEST_CASE("rule patterns: a fact a focus writes beside a rule the graph already has stays")
{
    // A rule statement that duplicates a rule present in the graph is
    // dropped along with all that its build created, and that took the
    // fact a focus wrote beside it: following "(a p b, X r Y) => (X q Y)",
    // the line "((x says *(a p b)), X r Y) => (X q Y)" kept no
    // `x says (a p b)`, and `a p b` stayed the rule's pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };
        interactive.process("(a p b, X r Y) => (X q Y)");

        SUBCASE("in the session")
        {
            process_lines(interactive, "((x says *(a p b)), X r Y) => (X q Y)\nk r l");
            interactive.run(true, false, false);
            CHECK(answers("X says Y") == std::vector<std::string>{"x says (a p b)"});
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            CHECK(answers("X q Y") == std::vector<std::string>{"k q l"});

            collector.clear();
            interactive.process(".stat");
            CHECK(any_output_contains(collector, "Rules: 1"));
        }
        // The fact leaves the rule's scratch for the cluster that was active,
        // so dropping that cluster still takes it back.
        SUBCASE("in a cluster that is dropped")
        {
            const std::size_t before = node_count(collector, interactive);
            interactive.process(".cluster c1");
            interactive.process("((x says *(a p b)), X r Y) => (X q Y)");
            CHECK(answers("X says Y") == std::vector<std::string>{"x says (a p b)"});

            interactive.process(".cluster-drop c1");
            CHECK(node_count(collector, interactive) == before);
            CHECK(answers("X says Y").empty());
            CHECK(answers("A p B").empty());
        } });
}

TEST_CASE("rule patterns: what a statement writes in a mentioned rule is not claimed")
{
    // The claim stops at a `=>`: the mentioned rule constitutes data the
    // statement refers to, and its parts are what that rule says. A part
    // that a typed rule marked before remains a pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("x about ((a p b) => (e r f))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a fact a focus writes inside a mentioned rule is not claimed")
{
    // A rule that a statement holds is mentioned, and nothing within it is
    // claimed. A focus within it still writes a fact beside the part it
    // hands on, and that fact constituted a claim:
    // "k notes ((x says *(a p b)) => (c q d))" answered `x says (a p b)` to
    // "X says Y". Within the condition of a typed rule, the fact was
    // additionally claimed with what it holds, thus `a p b`, a pattern of
    // the mentioned rule, answered as data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        std::size_t rules = 0;
        SUBCASE("in the condition") { interactive.process("k notes ((x says *(a p b)) => (c q d))"); }
        SUBCASE("in a condition of its comma list") { interactive.process("k notes ((x says *(a p b), Y r Z) => (c q d))"); }
        SUBCASE("in a rule that a typed rule's condition mentions")
        {
            interactive.process("(k notes ((x says *(a p b)) => (c q d))) => (m n o)");
            rules = 1;
        }
        SUBCASE("in a rule that a mentioned rule mentions") { interactive.process("k notes ((j says ((x says *(a p b)) => (c q d))) => (e f g))"); }
        // A generator's consequence is also a rule that has been mentioned,
        // and the rule that the firing derives from it does not hold the
        // fact that the focus wrote:
        // "(R is t) => ((x says *(a p b)) => (c R d))" claimed
        // `x says (a p b)` and, through this, `a p b`, which serves as a
        // condition for the rule the firing derives.
        SUBCASE("in the rule a generator's consequence holds")
        {
            process_lines(interactive, "(R is t) => ((x says *(a p b)) => (c R d))\nfoo is t");
            rules = 2;
        }
        interactive.run(true, false, false);

        CHECK(answers("X says Y").empty());
        CHECK(answers("A p B").empty());

        collector.clear();
        interactive.process(".explain (x says (a p b))");
        CHECK(any_output_contains(collector, "not asserted"));

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: " + std::to_string(rules))); });
}

// A mention marks only what it created: a fact the focus writes that was
// asserted before stays asserted, as every part of a mention does.
TEST_CASE("rule patterns: a fact a focus writes inside a mentioned rule keeps the claim it had")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x says (a p b)\nk notes ((x says *(a p b)) => (c q d))");

        collector.clear();
        interactive.process("X says Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x says (a p b)"});

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"}); });
}

TEST_CASE("rule patterns: the order of writing does not decide what a statement claims")
{
    // A part that stands both beside a mentioned rule and within it is
    // claimed by the statement, regardless of which of the two appears
    // first. The mention marks what it created once the statement ends, and
    // the claim of the statement subsequently revokes that mark, as the part
    // also stands outside the `=>`. If the mention appeared first, it
    // formerly created `a p b` and marked it, and nothing revoked the mark.
    // The consequence, which only the mentioned rule states, remains a
    // pattern irrespective of the order.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the plain part first")
        {
            interactive.process("(a p b) about ((a p b) => (c q d))");
        }
        SUBCASE("the mention first")
        {
            interactive.process("((a p b) => (c q d)) about (a p b)");
        }

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "a p b  [axiom]"));

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "c q d  [rule pattern; not asserted]")); });
}

TEST_CASE("rule patterns: a statement that mentions a rule with variables claims the facts nested in it")
{
    // The variables of the mentioned rule fail to turn the statement into a
    // query, meaning it claims what stands outside that rule. Read by
    // checking whether the statement held any variable, the claim of
    // `a p b` was skipped.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("(a p b) about ((X r Y) => (X s Y))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"}); });
}

TEST_CASE("rule patterns: a query claims none of the facts nested in it")
{
    // A query builds its pattern using identical calls to those used by a
    // claim, and that pattern says nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("S about ((a p b) q z)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d")); });
}

// Under a condition operator, a query constructs a pattern, and a query read
// from a file proceeds in the same manner as a typed one: via the statement,
// toward the marks located at its end.
TEST_CASE("rule patterns: a query in an imported file marks what it builds beneath an operator")
{
    const auto file = fs::temp_directory_path() / "zelph_query_marks_import.zph";
    {
        std::ofstream out(file);
        out << "t go k\nX go Y, ¬(a p b)\n";
    }

    run_both_modes([&file](auto& collector, auto& interactive)
                   {
        interactive.process(".import \"" + file.string() + "\"");

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]")); });

    fs::remove(file);
}

// A query executes within an isolated scratch cluster, which gets integrated
// into the cluster that was active before, and its marks are also recorded
// there: dropping a cluster takes back the entire query entered within it,
// including all patterns.
//
// The case pinned before that the drop leaves the node count it found. The
// query's marks are the session's first, so they create the predicate of the
// marks, `rule pattern`, within the cluster: a node from the engine's own
// vocabulary, which no cluster records any more (see "clusters: a vocabulary
// node the engine created inside a dropped cluster stays, with what refers to
// it outside"). It is the sole node the drop leaves, with no edge attached.
TEST_CASE("rule patterns: dropping a cluster takes a query's patterns with it, all but the predicate of the marks")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process("t go k");
        REQUIRE(graph->get_node("rule pattern", "zelph") == 0);
        const std::size_t before = node_count(collector, interactive);

        interactive.process(".cluster exp");
        interactive.process("X go Y, ¬(a p b)");
        interactive.process(".cluster-drop exp");

        CHECK(node_count(collector, interactive) == before + 1);
        const zelph::network::Node marks = graph->get_node("rule pattern", "zelph");
        REQUIRE(marks != 0);
        CHECK(graph->get_left(marks).empty());
        CHECK(graph->get_right(marks).empty());
        CHECK_THROWS(interactive.process(".node a p b")); });
}

TEST_CASE("rule patterns: the Janet read surface answers about claims")
{
    // The queries had taken this reading since the marking was introduced;
    // the Janet API kept the structural one, and SPARQL is built on it -- so
    // `SELECT ?s WHERE { ?s p b }` answered `a` for a graph in which nobody
    // had said `a p b`. All of it now asks the same question, and the
    // structural one keeps its own name.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b) => (c q d)
x p b
b p z
)");

        collector.clear();
        interactive.process(R"(%(string "SRC-" (string/join (sorted (map zelph/name (zelph/sources "p" "b"))) ",")))");
        CHECK(any_output_contains(collector, "SRC-x"));

        collector.clear();
        interactive.process(R"(%(string "TGT-" (string/join (sorted (map zelph/name (zelph/targets "a" "p"))) ",")))");
        CHECK(any_output_contains(collector, "TGT-"));
        CHECK_FALSE(any_output_contains(collector, "TGT-b"));

        // The closure must not walk THROUGH the pattern either: a would be an
        // ancestor of z only via the edge the rule merely writes down.
        collector.clear();
        interactive.process(R"(%(string "CLO-" (string/join (sorted (map zelph/name (zelph/closure-sources "z" "p"))) ",")))");
        CHECK(any_output_contains(collector, "CLO-b,x"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "p" "b") "/" (zelph/mentioned "a" "p" "b")))");
        CHECK(any_output_contains(collector, "EX-false/true"));

        // The control: the asserted neighbour answers both ways.
        collector.clear();
        interactive.process(R"(%(string "EX2-" (zelph/exists "x" "p" "b") "/" (zelph/mentioned "x" "p" "b")))");
        CHECK(any_output_contains(collector, "EX2-true/true")); });
}

// zelph/exists asks the question a ground rule condition asks, and the
// engine's bookkeeping is not there for either: the marking of a
// contradiction record, the membership of the record and the mark of a
// rule's pattern answered true, while `({(x p y) (x q y)} ~
// refuted) => (k r s)` did not fire. A refutation authored by someone
// provides an answer, just as a rule's text does, the tag of its
// condition set. zelph/sources stays the structural read surface that lists
// all of them.
TEST_CASE("rule patterns: zelph/exists finds no bookkeeping, as a ground rule condition does not")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_exists_bookkeeping.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
(a p b) => (c q d)
(X go Y, m r n) => (X ok Y)
¬(u p v)
)");
        SUBCASE("in the session") {}
        SUBCASE("after .save and .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            interactive.process(".auto-run"); // a load disables it
        }

        const auto exists_of = [&](const std::string& label, const std::string& predicate, const std::string& object)
        {
            collector.clear();
            interactive.process("%(each s (zelph/sources \"" + predicate + "\" \"" + object + "\") (zelph/out (string \"" + label
                                + " \" (string/format \"%q\" s) \" \" (zelph/exists s \"" + predicate + "\" \"" + object + "\"))))");
            std::vector<std::string> lines;
            for (const auto& e : collector.events())
                if (const std::string line = normalize(e.text); line.rfind(label + " ", 0) == 0) lines.push_back(line);
            std::sort(lines.begin(), lines.end());
            return lines;
        };

        CHECK(exists_of("REC", "~", "refuted") == std::vector<std::string>{"REC <zelph/node {(x p y) (x q y)}> false", "REC <zelph/node ¬(u p v)> true"});
        CHECK(exists_of("MARK", "~", "rule pattern") == std::vector<std::string>{"MARK <zelph/node a p b> false", "MARK <zelph/node c q d> false", "MARK <zelph/node m r n> false", "MARK <zelph/node ¬(u p v)> false"});
        const std::vector<std::string> tags = exists_of("TAG", "~", "conjunction");
        REQUIRE(tags.size() == 2);
        for (const std::string& tag : tags)
            CHECK_MESSAGE(tag.ends_with("> true"), tag);

        collector.clear();
        interactive.process(R"(%(each r (zelph/sources "~" "refuted") (each m (zelph/sources "in" r) (zelph/out (string "MEMBER " (zelph/exists m "in" r))))))");
        CHECK(any_output_contains(collector, "MEMBER false"));
        CHECK_FALSE(any_output_contains(collector, "MEMBER true"));

        interactive.process("({(x p y) (x q y)} ~ refuted) => (k r s)");
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty()); });
    std::filesystem::remove(file);
}

TEST_CASE("rule patterns: a fact asserted from Janet revokes the mark")
{
    // zelph/fact is the assertion API, so it claims the statement exactly as
    // typing it does -- but only the parser's top level used to say so. A
    // ground rule condition built from Janet therefore stayed a pattern,
    // unification skipped it, and the rule never fired. It went unnoticed
    // because zelph/exists answered true off the pattern the rule itself had
    // written; the polynomial suite asks precisely that question about the
    // all-ground pneg rule of stdlib/polynomial.zph.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process(R"(%(zelph/fact "a" "p" "b"))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "p" "b")))");
        CHECK(any_output_contains(collector, "EX-true"));

        // Building a RULE from Janet claims nothing: the same statement
        // written as a second rule's condition stays a pattern.
        interactive.process("(m r n) => (s t u)");
        collector.clear();
        interactive.process(R"(%(string "EX2-" (zelph/exists "m" "r" "n")))");
        CHECK(any_output_contains(collector, "EX2-false")); });
}

TEST_CASE("rule patterns: a fact asserted from a block keyword handler revokes the mark")
{
    // A handler delivers a response to a block of user input exactly as a
    // `%` block would, making its zelph/fact a claim. It executed using
    // whatever flag the enclosing input had retained: when typed at the
    // prompt, the handler's `a p b` stayed the rule's pattern and the rule
    // never fired; the same module claimed under `%(zelph/import ...)`
    // and not under `.import`; and with the handler's fact positioned
    // before the rule, the rule fired after all. The rule comes first
    // in every route here, the order in which the handler's fact formerly
    // remained a pattern.
    namespace fs           = std::filesystem;
    const fs::path module  = fs::temp_directory_path() / "zelph_test_claim_keyword.zph";
    const char*    handler = R"js(%(zelph/register-keyword "claimkw" (fn [t] (zelph/fact "a" "p" "b") nil)))js";
    {
        std::ofstream f(module);
        f << handler << "\n(a p b) => (c q d)\nclaimkw\n\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("typed at the prompt")
        {
            interactive.process(handler);
            interactive.process("(a p b) => (c q d)");
            interactive.process("claimkw");
            interactive.process("");

            collector.clear();
            interactive.process("C q D");
            CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"});
        }
        SUBCASE("the import route does not change it")
        {
            SUBCASE(".import")
            {
                interactive.process(".import \"" + module.generic_string() + "\"");
            }
            SUBCASE("zelph/import")
            {
                interactive.process(R"(%(zelph/import ")" + module.generic_string() + R"("))");
            }

            collector.clear();
            interactive.process("C q D");
            CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"});
        }
        SUBCASE("a nested marked part is claimed too")
        {
            // The nested claim of a `%` block's zelph/fact (as
            // demonstrated in the test below) applies equally to a handler.
            // The nested `a p b` comes from the thunk of `zelph/rule-text`,
            // which claims nothing, thus remaining the rule's pattern at
            // the moment the handler's outer zelph/fact receives it.
            interactive.process(R"js(%(zelph/register-keyword "nestkw" (fn [t] (zelph/fact (zelph/rule-text (fn [] (zelph/fact "a" "p" "b"))) "q" "z") nil)))js");
            interactive.process("(a p b) => (c q d)");
            interactive.process("nestkw");
            interactive.process("");

            collector.clear();
            interactive.process("A p B");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

            collector.clear();
            interactive.process("c q D");
            CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"});
        } });

    std::error_code ec;
    fs::remove(module, ec);
}

TEST_CASE("rule patterns: a fact asserted from Janet claims the facts nested in it")
{
    // `zelph/fact` claims the facts nested in what it asserts, much like a
    // typed statement does. A nested fact built by a prior `zelph/fact`
    // invocation within the block was claimed by that prior call; this one
    // comes from the thunk of `zelph/rule-text`, which claims nothing, thus
    // remaining the typed rule's pattern at the moment `zelph/fact` receives
    // it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process(R"(%(zelph/fact (zelph/rule-text (fn [] (zelph/fact "a" "p" "b"))) "q" "z"))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

// Replaces "rule patterns: a refutation claims the facts nested in it", which
// pinned that `¬(x says (a p b))` asserts `a p b` and fires the rule over it,
// because a refutation was read as a statement that claims what it nests. A
// refutation claims only its marking and nothing of its fact (Zelph::refute):
// the line says that x does not say `a p b`, and what that fact holds is
// mentioned, just as an argument of an opaque predicate would be. A rule's
// pattern stays a pattern, and any nested fact the refutation creates
// functions as a mention.
TEST_CASE("rule patterns: a refutation claims nothing nested in its fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        const auto refuted_by = [&](const bool rule)
        {
            SUBCASE("typed") { interactive.process("¬(x says (a p b))"); }
            SUBCASE("the marking line") { interactive.process("(x says (a p b)) ~ refuted"); }
            // The fact handed in unclaimed: the rule's pattern within the
            // rule's text, a new one as a mention.
            SUBCASE("from Janet")
            {
                interactive.process(rule ? R"(%(zelph/refute "x" "says" (zelph/rule-text (fn [] (zelph/fact "a" "p" "b")))))"
                                         : R"(%(zelph/refute "x" "says" (zelph/mention "a" "p" "b")))");
            }
            interactive.run(true, false, false);

            CHECK(answers("X ~ refuted") == std::vector<std::string>{"(x says (a p b)) ~ refuted"});
            CHECK(answers("A p B").empty());
            CHECK(answers("C q D").empty());
            CHECK(answers("A says B").empty());

            CHECK_NOTHROW(interactive.process("a p b"));
            interactive.run(true, false, false);
            CHECK(answers("A p B") == std::vector<std::string>{"a p b"});
            if (rule) CHECK(answers("C q D") == std::vector<std::string>{"c q d"});
        };
        SUBCASE("a rule's pattern")
        {
            interactive.process("(a p b) => (c q d)");
            refuted_by(true);
        }
        SUBCASE("a new fact") { refuted_by(false); } });
}

TEST_CASE("rule patterns: a mention built with zelph/fact claims its parts")
{
    // The contract of the assertion API, pinned beside the typed mention,
    // which claims nothing: every zelph/fact call within a Janet block
    // constitutes a claim, and the inner calls execute before the `=>` call,
    // so nothing can know at that point that their outcomes will form the
    // parts of a rule. The mentioned rule is not in force.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(zelph/fact "x" "noted" (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d"))))");

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"});

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found.")); });
}

TEST_CASE("rule patterns: a mention built inside zelph/build-rule or zelph/rule-text claims none of its parts")
{
    // The method for composing an unclaimed mention from Janet. Either form
    // runs its calls as the rule's text, where zelph/fact claims nothing, and
    // marks the ground parts they created. Previously, zelph/rule-text
    // operated by running them as a block, thereby treating them as claims,
    // and it marked only the memberships within the collections they wrote.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto unclaimed = [&]
        {
            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S p O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S noted O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"x noted ((a p b) => (c q d))"});

            collector.clear();
            interactive.process(".explain (c q d)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        };

        SUBCASE("zelph/build-rule")
        {
            interactive.process(R"(%(zelph/fact "x" "noted" (zelph/build-rule (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d"))))))");
            unclaimed();
        }
        SUBCASE("zelph/rule-text")
        {
            interactive.process(R"(%(zelph/fact "x" "noted" (zelph/rule-text (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d"))))))");
            unclaimed();
        }
        SUBCASE("zelph/rule-text around the statement that names the rule")
        {
            // Each rule created by the thunk is marked, just as in a firing,
            // and not merely a rule that the thunk returns: this thunk returns
            // the statement.
            interactive.process(R"(%(zelph/rule-text (fn [] (zelph/fact "x" "noted" (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d"))))))");
            unclaimed();
        }
        SUBCASE("zelph/rule-text around that statement and around the rule")
        {
            // The inner call operates inside the build of the outer one and
            // leaves the marking to it, whereas the outer call returns the
            // statement.
            interactive.process(R"(%(zelph/rule-text (fn [] (zelph/fact "x" "noted" (zelph/rule-text (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d"))))))))");
            unclaimed();
        } });
}

TEST_CASE("rule patterns: a fact written in the thunk of zelph/rule-text revokes no mark")
{
    // The calls within the thunk write the text of a rule, even within a Janet
    // block, where zelph/fact otherwise claims ("a fact asserted from Janet
    // revokes the mark"). Run as a part of the block, the thunk claimed the
    // condition of a typed rule by writing it again, and the rule fired.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process(R"(%(zelph/rule-text (fn [] (zelph/fact "a" "p" "b"))))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a mention whose thunk fails leaves nothing")
{
    // A failed mention retains nothing that its thunk wrote, just as a failed
    // rule build does. After merging, the parts stayed in the graph unmarked,
    // as claims that nobody had made: `a p b` was answered after the error.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        CHECK_THROWS(interactive.process(R"(%(zelph/rule-text (fn [] (zelph/fact (zelph/fact "a" "p" "b") "=>" (zelph/fact "c" "q" "d")) (error "stop"))))"));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: zelph/rule marks what its argument forms built")
{
    // Typed, `(X p k) => (X likes (k is green))` maintains `k is green` as a
    // pattern. Constructed using zelph/rule, it became data the moment the
    // rule was written: `A is green` answered it and .explain designated it
    // as an axiom. In a Janet function, arguments are evaluated prior to the
    // call, so the function cannot tell which of the nodes it receives the
    // call built. zelph/rule operates as a macro, so its argument forms
    // execute within the rule's build, and whatever they build into the rule
    // -- during their dynamic extent, including any helper they invoke --
    // constitutes the rule's text, just as the parser's does. A node that
    // predated the call was not formed at that moment and remains unchanged.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto build = [&interactive]
        { interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (zelph/fact "k" "is" "green"))))js"); };

        SUBCASE("a part the argument forms built is a pattern until a firing claims it")
        {
            build();
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".explain (k is green)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
            CHECK_FALSE(any_output_contains(collector, "[axiom]"));

            // The derived `a likes (k is green)` claims the fact nested in
            // it, just as the identical statement, when typed, does; the part
            // used to remain a pattern following the firing.
            collector.clear();
            interactive.process("a p k");
            CHECK(any_deduction_of(collector, "a likes (k is green)"));

            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k is green"});
        }

        SUBCASE("so is a part that a helper called in the argument forms built")
        {
            interactive.process(R"js(%(defn green [] (zelph/fact "k" "is" "green")))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (green))))js");
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("so are the cons cells of a list built there")
        {
            // As in the typed `(X p k) => (X has <ab>)`.
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "has" (zelph/list-chars "ab"))))js");
            collector.clear();
            interactive.process("A cons B");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("a fact a helper asserts beside the part it returns is a claim")
        {
            // The build marks the parts of the rule, not every component
            // generated by its argument forms: the rule's `=>` facts say
            // which nodes it holds. A helper that asserts a single fact
            // and yields a different one builds solely the returned one
            // into the rule. The other remains a statement within the
            // program, just like any zelph/fact inside a block, and
            // marking it would conceal an actual assertion.
            interactive.process(R"js(%(defn green [] (zelph/fact "k" "is" "colour") (zelph/fact "k" "is" "green")))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (green))))js");
            collector.clear();
            interactive.process("k is C");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k is colour"});

            collector.clear();
            interactive.process(".explain (k is colour)");
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "rule pattern"));

            collector.clear();
            interactive.process(".explain (k is green)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }

        SUBCASE("a part bound before the call stays a claim")
        {
            interactive.process(R"js(%(let [g (zelph/fact "k" "is" "green")] (zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" g))))js");
            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
        }

        SUBCASE("a part asserted before stays a claim")
        {
            interactive.process("k is green");
            build();
            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
        }

        SUBCASE("claiming the part afterwards revokes the mark")
        {
            build();
            interactive.process("k is green");
            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
        }

        SUBCASE("building the part there revokes no other rule's mark")
        {
            // A rule's text claims nothing. When typed following the rule
            // below, `(X q k) => (X loves (k is green))` leaves `k is green`
            // a pattern, and the identical rule constructed using zelph/rule
            // must do the same: the zelph/fact calls within its argument
            // forms execute during rule construction, thereby writing the
            // pattern without claiming it.
            interactive.process("(X p k) => (X likes (k is green))");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "q" "k")] (zelph/fact 'X "loves" (zelph/fact "k" "is" "green"))))js");
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".explain (k is green)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }

        SUBCASE("the build hands what it made to the cluster that was active")
        {
            // The build executes within a dedicated scratch cluster. What
            // it retains is transferred to the user's cluster, ensuring
            // that .cluster-drop continues to take the rule back alongside
            // the rest of the experiment.
            interactive.process(".cluster exp");
            build();
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "active=" (or (zelph/cluster) "none"))))js");
            CHECK(any_output_contains(collector, "active=exp"));

            interactive.process(".cluster-drop exp");
            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "likes"));
        } });
}

TEST_CASE("rule patterns: naming a rule does not claim what it says")
{
    // The counterpart of the case above, and the reason the revocation in
    // zelph/fact is suppressed for parser-generated code: a typed statement
    // builds its SUBTERMS through the very same call, and a subterm is not
    // claimed by the statement that names it. Writing something ABOUT a rule
    // must leave the rule's patterns exactly where they were -- otherwise
    // mentioning a rule silently turns its condition and its consequence
    // into data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("x documents ((a p b) => (c q d))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b"));

        // The statement itself is data, of course.
        collector.clear();
        interactive.process("X documents Y");
        CHECK(collect_answers(collector).size() == 1); });
}

TEST_CASE("rule patterns: a mentioned ground rule claims none of its parts")
{
    // Mentioning a rule does not assert the rule itself nor what it says. The
    // parser constructed the parts of a mention as plain nested facts, meaning
    // that `((a p b) => (a likes b)) is noted` answered `a likes b` and
    // `a p b`, and `.explain` called both axioms, even though neither had been
    // claimed and the rule is not in force. The mention marks the parts it
    // created, just as a typed rule would; the statement regarding the rule
    // constitutes data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((a p b) => (a likes b)) is noted");

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"((a p b) => (a likes b)) is noted"});

        collector.clear();
        interactive.process(".explain (a likes b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "likes" "b") "/" (zelph/mentioned "a" "likes" "b")))");
        CHECK(any_output_contains(collector, "EX-false/true")); });
}

TEST_CASE("rule patterns: a rule in a comma list a statement writes claims none of its parts")
{
    // A comma list built its members via zelph/fact, including a rule among
    // them, thus `x says ((m n o) => (c q d), j r s)` answered `c q d` and
    // `m n o` as axioms, whereas the set spelling of the line,
    // `x says {((m n o) => (c q d)) (j r s)}`, left both patterns. The member
    // is a rule that another statement holds, also when the comma list
    // stands on a line of its own: the comma list holds it, hence it is not
    // in force.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("nested in a statement") { interactive.process("x says ((m n o) => (c q d), j r s)"); }
        SUBCASE("on a line of its own") { interactive.process("(m n o) => (c q d), j r s"); }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("M n O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("J r S");
        CHECK(collect_answers(collector) == std::vector<std::string>{"j r s"});

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("rule patterns: a mentioned rule with variables claims none of its ground parts")
{
    // A proposal recorded but not in force: its condition holds a variable,
    // its consequence is ground, and `S is O` answered `alarm is on` as though
    // the alarm were active. A ground condition paired with conditions
    // involving variables was answered in the same way.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((X is bad) => (alarm is on)) "was proposed by" alice
((X p Y, a r b) => (X q Y)) is noted
)");

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a focus on a mentioned rule's arrow mentions the rule")
{
    // A focus on the arrow names the rule within a statement as well. Read as
    // a focus, the mentioned rule functioned as a statement on the side: the
    // line said `=> is noted`, and `zelph/fact` put the rule in force with its
    // ground parts claimed, thus a rule that was merely to be noted derived
    // what it says.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a ground rule")
        {
            interactive.process("((a p b) *=> (a likes b)) is noted");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S is O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"((a p b) => (a likes b)) is noted"});

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S p O");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a rule with variables")
        {
            interactive.process("x says ((X is bad) *=> (alarm is on))");
            interactive.process("smoke is bad");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S is on");
            CHECK(collect_answers(collector).empty());
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("rule patterns: a focus on a part of a rule a statement holds is refused")
{
    // A focus on a part of the mentioned rule handed that part to the
    // statement, leaving the rule beside it, named by nothing and thus in
    // force: `y says ((X p Y) => *(X q Y))` said `y says (X q Y)` and listed
    // the rule, a second instance when typed again under different names,
    // and `x says ((a p b) => *(c q d))` derived what a rule within a
    // statement merely states. A rule located inside another statement is
    // never asserted, hence every spelling is refused, and nothing it would
    // have constructed remains.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto refusal = [&](const std::string& line)
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::runtime_error& e)
            {
                return std::string(e.what());
            }
            return std::string{};
        };
        const std::size_t before = node_count(collector, interactive);

        std::string line;
        SUBCASE("on the consequence") { line = "y says ((X p Y) => *(X q Y))"; }
        // The identical focus within a group of its own: the group hid it,
        // and the line mentioned the plain rule.
        SUBCASE("on the consequence in a group of its own") { line = "y says ((X p Y) => (*(X q Y)))"; }
        SUBCASE("on a ground consequence") { line = "x says ((a p b) => *(c q d))"; }
        SUBCASE("on the comma list of conditions") { line = "x says (*(X R Y, Y R Z) => (X R Z))"; }
        SUBCASE("on the condition of a rule a set holds") { line = "k has {(*(X p Y) => (X q Y))}"; }
        SUBCASE("on the consequence of a rule a comma list holds") { line = "x says ((m n o) => *(c q d), j r s)"; }
        SUBCASE("on the conditions of a rule in a rule's condition") { line = "(*(X R Y, Y R Z) => (X R Z)) => (R is t)"; }
        // A refutation holds the rule it refutes, and `¬` built that rule
        // outside the parser's held-rule case: the focus handed its part to
        // the line, which claimed it, thus `¬((a p b) => *(c q d))` refuted
        // the rule and answered `c q d` as an axiom.
        SUBCASE("on the consequence of a rule a refutation holds") { line = "¬((a p b) => *(c q d))"; }
        SUBCASE("on the conditions of a rule a refutation holds") { line = "¬(*(a p b, c q d) => (e f g))"; }

        CHECK(refusal(line).find("a focus on one of its parts") != std::string::npos);
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// `*{...} ~ conjunction` is the comma list expressed in complete form
// (concepts.md): its focus makes the condition the set and hands nothing to
// the statement that holds the rule. A held rule expressed in this manner
// constitutes a mention akin to its comma-list spelling, thus the refusal of a
// focus on a held rule's part must not extend to it -- a refusal affecting
// every focus within a held rule would.
TEST_CASE("rule patterns: a focus that writes out a held rule's comma list is no focus on its part")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_NOTHROW(interactive.process("x says ((*{(X R Y) (Y R Z)} ~ conjunction) => (X R Z))"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("rule patterns: a focus on one condition of a held rule's comma list is refused")
{
    // A focus on a single condition within a held rule's comma list had no
    // effect and was dropped silently: the line mentioned the plain rule.
    // It is refused just as a focus on any other part of a held rule is,
    // before anything is built.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto refusal = [&](const std::string& line)
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::runtime_error& e)
            {
                return std::string(e.what());
            }
            return std::string{};
        };
        const std::size_t before = node_count(collector, interactive);

        std::string line;
        SUBCASE("with variables") { line = "x says ((*(X R Y), Y R Z) => (X R Z))"; }
        SUBCASE("ground") { line = "x says ((*(a p b), c q d) => (e f g))"; }
        SUBCASE("in a group") { line = "x says (((*(X R Y)), Y R Z) => (X R Z))"; }
        SUBCASE("in a rule a comma list holds") { line = "x says ((*(m n o), j k l) => (c q d), j r s)"; }
        SUBCASE("in a rule a refutation holds") { line = "¬((*(a p b), c q d) => (e f g))"; }

        CHECK(refusal(line).find("carries a focus") != std::string::npos);
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("rule patterns: a focus on one condition of a comma list is refused outside a held rule too")
{
    // A comma list evaluates to the list, meaning that a focus on one
    // of its conditions as a whole had no effect and was dropped without
    // comment: "x says (*(a p b), c q d)" and "*(a p b), c q d" both
    // asserted the conditions, and the typed rule built the plain rule.
    // Refused before anything is built, just as in the held rule above.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto refusal = [&](const std::string& line)
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::runtime_error& e)
            {
                return std::string(e.what());
            }
            return std::string{};
        };
        const std::size_t before = node_count(collector, interactive);

        std::string line;
        SUBCASE("held by a statement") { line = "x says (*(a p b), c q d)"; }
        SUBCASE("on a line of its own") { line = "*(a p b), c q d"; }
        SUBCASE("in a group") { line = "((*(a p b))), c q d"; }
        SUBCASE("in a typed rule") { line = "(*(X R Y), Y R Z) => (X R Z)"; }
        SUBCASE("in a query") { line = "*(X R Y), Y R Z"; }

        CHECK(refusal(line).find("Condition 1 of the comma list carries a focus") != std::string::npos);
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("rule patterns: a focus directly inside a negation is refused")
{
    // "¬" negates the entire pattern that follows, meaning any focus
    // placed there had no effect and was dropped without comment: "(¬*(X R
    // Y), Y R Z) => (X R Z)" built the rule using "¬(X R Y)", whether
    // typed or held. A focus situated further within the pattern hands on
    // a part just as it would anywhere else and is not rejected in this
    // context.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto refusal = [&](const std::string& line)
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::runtime_error& e)
            {
                return std::string(e.what());
            }
            return std::string{};
        };
        const std::size_t before = node_count(collector, interactive);

        std::string line;
        SUBCASE("in a typed rule's comma list") { line = "(¬*(X R Y), Y R Z) => (X R Z)"; }
        SUBCASE("in a held rule's comma list") { line = "x says ((¬*(X R Y), Y R Z) => (X R Z))"; }
        SUBCASE("as the condition of a held rule") { line = "x says (¬*(X R Y) => (X R Z))"; }
        SUBCASE("in a group") { line = "x says ((Y R Z, ¬(*(X R Y))) => (X R Z))"; }
        SUBCASE("in a query") { line = "S q O, ¬*(S p b)"; }

        CHECK(refusal(line).find("A focus directly inside \"¬\" changes nothing") != std::string::npos);
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("rule patterns: a part asserted before the mention stays a claim")
{
    // The mention marks only what it created, just as a typed rule does
    // ("asserting the statement first keeps it data"): `a p b` was claimed
    // before the rule was mentioned, and the reuse of its node through mention
    // alters nothing regarding that.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("((a p b) => (c q d)) is noted");

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[axiom]")); });
}

TEST_CASE("rule patterns: asserting a part after the mention claims it")
{
    // A claim revokes the mark, just as it does for a typed rule's pattern.
    // The rule remains mentioned and does not fire, so its consequence
    // persists as a pattern: it was answered earlier, since the mention had
    // written it as a fact.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((a p b) => (c q d)) is noted");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a typed rule over a comma list does not fire on what a mention wrote")
{
    // The typed rule functions as an independent node, because every statement
    // constructs a new conjunction set from a comma list, and it is in force.
    // It was triggered by `a p b` and `c q d`, which had been written
    // exclusively by the prior mention, and `.explain (e r f)` named both as
    // axioms. Claimed, the conditions are satisfied and the rule fires.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((a p b, c q d) => (e r f)) is noted");
        interactive.process("(a p b, c q d) => (e r f)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (e r f)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        process_lines(interactive, R"(
a p b
c q d
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e r f"}); });
}

TEST_CASE("rule patterns: a rule held as an argument matches a ground rule with a comma list by its conditions")
{
    // Every statement constructs its own set from a comma list, meaning the
    // rule that the rule over rules holds and the rule a given statement
    // proposes are distinct nodes. Compared by identity, as a rule with a
    // single condition is compared (hash-consing makes those a single node),
    // the rule over rules never fired. The sets are compared based on their
    // members: a rule differing in any condition, in the number of its
    // conditions, or in its consequence constitutes a different rule.
    // `.explain` and `.prune-facts` searched for the pattern's own instance,
    // which no statement holds, and found nothing; the anchors took the held
    // rule as a node that the candidates must contain.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("anchors on") {}
        SUBCASE("anchors off") { interactive.process(".anchors off"); }

        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };

        process_lines(interactive, R"(
(P proposed ((a p b, c q d) => (alarm is on))) => (P flagged k)
x proposed ((c q d, a p b) => (alarm is on))
y proposed ((a p b, c r d) => (alarm is on))
z proposed ((a p b, c q d, e r f) => (alarm is on))
w proposed ((a p b) => (alarm is on))
v proposed ((a p b, c q d) => (alarm is off))
)");
        interactive.run(true, false, false);
        CHECK(answered("S flagged O") == std::vector<std::string>{"x flagged k"});

        collector.clear();
        interactive.process(".explain (x flagged k)");
        CHECK(any_output_contains(collector, "└─ x proposed"));
        CHECK(any_output_contains(collector, "[axiom]"));
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));

        CHECK(answered("S proposed ((a p b, c q d) => (alarm is on))").size() == 1);
        collector.clear();
        interactive.process(".prune-facts S proposed ((a p b, c q d) => (alarm is on))");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts."));
        CHECK(answered("S proposed O").size() == 4); });
}

TEST_CASE("rule patterns: the conditions of a held rule pair with a stored rule's in every way that fits")
{
    // A variable of the rule over rules, located inside the held rule, binds a
    // part of the stored rule, just as it does in a held rule featuring a
    // single condition: the held `(X p Y) => (X r Y)` matches the stored
    // `(a p b) => (a r b)`. With a comma list, which condition pairs with which
    // is a decision too, and the rest can hold for only one pairing. Inside the
    // held rule, the consequence decides; outside it, a guard or another
    // condition does, and because the engine does not backtrack into a match,
    // each pairing is offered as a match of its own, much like extract_bindings
    // offers each pairing of a fact's objects. A stored rule that shares a
    // variable differently, or positions its parts in a different arrangement,
    // constitutes a different rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };

        // For each shape there are two statements or two rules, needing one
        // pairing and the other: the order in which the pairings appear is
        // determined by the node ids, and each pairing comes second for one of
        // the two. A guard is read after the conditions, thus it cannot decide
        // the pairing beforehand.
        process_lines(interactive, R"(
(Z says ((X p Y, X p W) => (X r Y))) => (Z fine Y)
(Z says ((X p Y, X p W) => (X r k)), Y != b) => (Z ok Y)
(Z says ((X p Y, X p W) => (X r k)), Y != c) => (Z yes Y)
(Z says ((X p Y, X q Y) => (X r Y))) => (Z shares X)
bob says ((a p b, a p c) => (a r c))
gus says ((a p c, a p b) => (a r b))
carl says ((a p b, a p c) => (a r k))
dan says ((a p b, a q b) => (a r b))
eve says ((a p b, c q b) => (a r b))
fay says ((a p b, a q b) => (b r a))
)");
        interactive.run(true, false, false);
        CHECK(answered("S fine O") == std::vector<std::string>{"bob fine c", "gus fine b"});
        CHECK(answered("S ok O") == std::vector<std::string>{"carl ok c"});
        CHECK(answered("S yes O") == std::vector<std::string>{"carl yes b"});
        CHECK(answered("S shares O") == std::vector<std::string>{"dan shares a"}); });
}

TEST_CASE("rule patterns: a rule over rules whose condition is a rule with a comma list reads each rule with these conditions")
{
    // `(G => H) => (G seen H)` reads each rule; a condition that names the rule
    // reads the rules it names, whether typed or mentioned, in the same manner
    // as a ground rule with a single condition does via its node. The rule
    // containing the comma list got its own set and found only itself. The
    // statement in which a rule is typed evaluates it as a query too, and in
    // that context keeps naming its own conditions.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((a p b, c q d) => (alarm is on)) => (k seen yes)");
        SUBCASE("a typed rule")
        {
            interactive.process("(c q d, a p b) => (alarm is on)");
        }
        SUBCASE("a mentioned rule")
        {
            interactive.process("x proposed ((c q d, a p b) => (alarm is on))");
        }
        interactive.process("(a p b, c r d) => (alarm is on)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k seen yes"});

        collector.clear();
        interactive.process(".explain (k seen yes)");
        CHECK(any_output_contains(collector, "=> (alarm is on)  [axiom]")); });
}

TEST_CASE("rule patterns: a variable bound to a rule with a comma list stands for that node")
{
    // Matching by the members of a set applies to a rule that the pattern
    // holds. A variable stands for the node it is bound to, and the anchors
    // read the candidates located at that node: two statements that each write
    // the rule using a comma list hold two nodes, which
    // `(bob says V, carl says V)` does not join, in either anchor mode. A rule
    // containing a single condition corresponds to one node, and the join
    // operation finds it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("anchors on") {}
        SUBCASE("anchors off") { interactive.process(".anchors off"); }

        process_lines(interactive, R"(
(bob says V, carl says V) => (V agreed k)
bob says ((a p b, c q d) => (alarm is on))
carl says ((c q d, a p b) => (alarm is on))
bob says ((a p b) => (alarm is off))
carl says ((a p b) => (alarm is off))
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S agreed O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"((a p b) => (alarm is off)) agreed k"}); });
}

TEST_CASE("rule patterns: the parts of a mention nested deeper or in a set are patterns")
{
    // The parser generates a mention for every `=>` found within another
    // statement, regardless of its position. A rule that is mentioned inside a
    // mentioned rule is a part of the outer rule, which marks both. The plain
    // nested fact around a mention remains unchanged: it is asserted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        SUBCASE("several conditions")
        {
            interactive.process("((a p b, c q d) => (e r f)) is noted");
            CHECK(answered("S p O").empty());
            CHECK(answered("S q O").empty());
            CHECK(answered("S r O").empty());
        }
        SUBCASE("a member of a set")
        {
            interactive.process("x has {((a p b) => (c q d)) k}");
            CHECK(answered("S p O").empty());
            CHECK(answered("S q O").empty());
        }
        SUBCASE("inside a nested fact")
        {
            interactive.process("alice says (bob believes ((a p b) => (c q d)))");
            CHECK(answered("S p O").empty());
            CHECK(answered("S q O").empty());
            CHECK(answered("S believes O") == std::vector<std::string>{"bob believes ((a p b) => (c q d))"});
        }
        SUBCASE("a member of a collection")
        {
            interactive.process("x has @{((a p b) => (c q d))}");
            CHECK(answered("S p O").empty());
            CHECK(answered("S q O").empty());
        }
        SUBCASE("a mention inside a mention")
        {
            interactive.process("((a p b) => ((c q d) => (e r f))) is noted");
            CHECK(answered("S p O").empty());
            CHECK(answered("S q O").empty());
            CHECK(answered("S r O").empty());
        }
        SUBCASE("a mention inside a mention, read by a rule over rules")
        {
            // The inner rule constitutes a part of the outer rule and is
            // marked with it, as a rule that a typed generator writes is. The
            // rule over rules reads only the outer rule, and what it derives
            // claims none of the parts it bound ("rule patterns: a rule over
            // rules asserts nothing of the rule it reads"): `a p b` stays a
            // pattern, and the inner rule continues to be a part of the outer
            // one, which the rule over rules does not read. A derivation that
            // claimed the values it bound made both data, and the rule over
            // rules then read the inner rule as well.
            interactive.process("(G => H) => (G seen H)");
            interactive.process("((a p b) => ((c q d) => (e r f))) is noted");
            interactive.run(true, false, false);
            CHECK(answered("S seen O") == std::vector<std::string>{"(a p b) seen ((c q d) => (e r f))"});
            CHECK(answered("S p O").empty());
            CHECK(answered("S r O").empty());
        } });
}

TEST_CASE("rule patterns: a read-only command does not claim the pattern it asks about")
{
    // .explain, .node and the prune commands parse their argument into the
    // same zelph/fact calls a statement does, inside a scratch cluster they
    // drop again. That is a QUESTION about the graph, not a claim -- but the
    // revocation keyed on it and the answer changed the answer: asking
    // ".explain (a p b)" about a rule's pattern turned it into data, so the
    // command reported "[axiom]" for a statement nobody had made and the
    // query started answering it afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "rule pattern"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "p" "b")))");
        CHECK(any_output_contains(collector, "EX-false")); });
}

TEST_CASE("rule patterns: a read-only command that parses a mention leaves no mark")
{
    // As the .explain command builds its argument, the mention within that
    // argument writes its parts into the scratch cluster which the command
    // drops afterwards, and it marks nothing: `a p b`, later asserted as a
    // plain nested fact, represents a new node bearing the prior identifier,
    // and a mark left in the index would hide the claim.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".explain (x noted ((a p b) => (c q d)))");
        interactive.process("x says (a p b)");

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[axiom]")); });
}

// A command refers to a node, and a condition operator expressed in full
// refers to one as well -- `.node (a p b) ~ negation` is the way a rule's
// negated condition is looked at -- thus, a command's pattern does not
// constitute a statement that asserts it. What the pattern constructs is
// discarded along with the command's scratch cluster and leaves no mark: the
// fact nested beneath it becomes data once a statement claims it.
TEST_CASE("rule patterns: a command's operator pattern is no assertion and leaves no mark")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X go Y, ¬(a p b)) => (X ok Y)");

        collector.clear();
        CHECK_NOTHROW(interactive.process(".node (a p b) ~ negation"));
        CHECK(any_output_contains(collector, "Representation: (a p b) ~ negation"));

        CHECK_NOTHROW(interactive.process(".explain ¬(X q (c r d))"));
        CHECK_NOTHROW(interactive.process(".explain (c r d) ~ negation"));
        interactive.process("x says (c r d)");

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c r d"});
        collector.clear();
        interactive.process(".explain (c r d)");
        CHECK(any_output_contains(collector, "[axiom]")); });
}

TEST_CASE("rule patterns: a query that names a mention leaves no mark")
{
    // A query builds its pattern through the calls a claim makes, and nothing
    // drops what it has assembled. Its mention marked the parts it created,
    // the marks persisted beyond the query's lifetime, and they hid `a r b`
    // from the statement that wrote it afterwards: `x says (a r b)` was not
    // answered, and the rule over it did not fire. Only the end of a statement
    // shows that it asks, hence the marks wait for it. What a query leaves
    // within the graph is the same as without the mention: nested facts of a
    // query are written as data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto claimed_afterwards = [&]
        {
            interactive.process("x says (a r b)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S r O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a r b"});

            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"a seen b"});

            collector.clear();
            interactive.process(".explain (a r b)");
            CHECK(any_output_contains(collector, "[axiom]"));
        };

        interactive.process("(X r Y) => (X seen Y)");

        SUBCASE("a query")
        {
            interactive.process("S noted ((a r b) => (c q d))");
            claimed_afterwards();
        }
        SUBCASE("a query after the ? prefix")
        {
            interactive.process("? S noted ((a r b) => (c q d))");
            claimed_afterwards();
        }
        SUBCASE("a variable that the query shares with the rule it names")
        {
            // The resolution of X occurs first within the mention, and then
            // again through the statement that surrounds it: it is this second
            // use that makes the statement ask.
            interactive.process("X noted ((X q b) => (a r b))");
            claimed_afterwards();
        } });
}

TEST_CASE("rule patterns: a ground request after ? is asserted with the mention in it")
{
    // `?` asserts the statement it asks about before it asks (Interactive),
    // so a ground request marks the parts of the rule it mentions, just as
    // the identical statement typed alone does. Entered afterwards,
    // `x says (a r b)` claims the part it holds, as each statement claims the
    // facts nested in it outside a `=>`. Previously, it left the part a
    // pattern: only the statement's own node was claimed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X r Y) => (X seen Y)");
        interactive.process("? x noted ((a r b) => (c q d))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (a r b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));

        interactive.process("x says (a r b)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a r b"});

        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a seen b"});

        collector.clear();
        interactive.process(".explain (a r b)");
        CHECK(any_output_contains(collector, "a r b  [axiom]")); });
}

TEST_CASE("rule patterns: a propositional rule reaches its conclusion")
{
    // Both halves are needed for this one to be observable at all: the
    // ground condition has to be allowed to match (see "rules: a ground
    // condition is not a non-match" in test_reasoning.cpp), and the ground
    // consequence has to be a pattern rather than a fact, or it would have
    // been there before the rule ever fired.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(a p b) => (c q d)");
        collector.clear();
        interactive.process("a p b");
        CHECK(any_deduction_of(collector, "c q d"));

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: a pattern with variables is untouched")
{
    // The control. Nothing about the marking may reach the ordinary case,
    // where the template check has always done the work.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p B) => (A q B)");
        interactive.process("x p y");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "x q y"));

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "x p y")); });
}

TEST_CASE("rule patterns: a mentioned rule whose variables sit in its conditions is not data")
{
    // `((X p Y) => (c q d)) is noted` carries the variables from its
    // condition and functions as a pattern. Its twin with two conditions
    // read as a ground statement: the conditions hang off a container, no
    // hash, and var_in_closure stopped there -- thus `S is O` answered a
    // mention with X and Y unbound, and the same occurred on the read
    // surface. Both twins serve as rule text for either.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((X p Y, X r Y) => (c q d)) is noted
((X p Y) => (c q d)) is noted
)");

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S is noted");
        CHECK(collect_answers(collector).empty());

        // The Janet read surface bypasses the same elements that queries
        // bypass, by taking a snapshot before adjacency locks
        // (Zelph::unasserted_snapshot).
        collector.clear();
        interactive.process(R"(%(string "SRC-" (length (zelph/sources "is" "noted"))))");
        CHECK(any_output_contains(collector, "SRC-0")); });
}

TEST_CASE("rule patterns: a firing does not derive its own consequence pattern while it holds a variable")
{
    // A firing keeps the condition set of a rule its consequence mentions
    // as it stands, including W, X, and Y, and this consequence holds
    // nothing else to substitute, so its instance is the rule's own
    // consequence pattern containing variables: rule text, which no query
    // answers and `.explain` calls not asserted. An engine that claims that
    // node prints a deduction contradicting its own answers. The firing
    // writes the witness alone, and subsequent passes contribute nothing,
    // since the pattern is the instance and counts as present.
    //
    // The control: a ground consequence is the rule's own pattern as well,
    // yet it holds no variable, and the act of deriving it transforms it
    // into data. Here it is positioned next to a fresh variable that
    // already possesses a witness, thus the check for a prior witness must
    // count it as missing, otherwise the firing that derives it is skipped.
    // The firing alone, without a fresh variable, is held by "rule
    // patterns: a propositional rule reaches its conclusion".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        // Single passes, as counts_over_passes runs them: an engine that
        // counts the mention's pattern as missing, or one that fails to
        // claim the ground consequence, makes a new witness with each
        // pass, and a run to the fixpoint would not end.
        interactive.process(".semi-naive off");
        interactive.process(".auto-run"); // a toggle: off

        SUBCASE("a mention whose variables the firing leaves to the mentioned rule")
        {
            interactive.process("(X p Y) => (((W r X, W s Y) => (c q d)) is noted) (W t X)");
            interactive.process("a p b");
            collector.clear();
            interactive.process(".run-once");
            CHECK(any_deduction_of(collector, "?? t a"));
            CHECK_FALSE(any_deduction_of(collector, "is noted"));

            const std::size_t        after = node_count(collector, interactive);
            std::vector<std::size_t> counts;
            for (int pass = 0; pass < 4; ++pass)
            {
                interactive.process(".run-once");
                counts.push_back(node_count(collector, interactive));
            }
            CHECK(counts == std::vector<std::size_t>(4, after));

            collector.clear();
            interactive.process("S is O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".explain (((W r X, W s Y) => (c q d)) is noted)");
            CHECK(any_output_contains(collector, "Fact is not asserted -- nothing to explain."));
        }
        SUBCASE("a ground consequence beside a fresh variable with a witness")
        {
            interactive.process("(A is m) => (k about z) (N near A)");
            interactive.process("z near g");
            interactive.process("g is m");
            collector.clear();
            interactive.process(".run-once");
            CHECK(any_deduction_of(collector, "k about z"));

            collector.clear();
            interactive.process("S about O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k about z"});
        } });
}

TEST_CASE("rule patterns: a firing does not derive a fact that keeps a variable of a rule it mentions")
{
    // A firing keeps the condition set of a rule its consequence mentions,
    // exactly as it stands. If that set holds a variable, the instance
    // becomes rule text: no query answers it, and where the firing would
    // have substituted into the mentioned rule's conditions, it constitutes
    // half of a rule that nobody wrote. Such a consequence is not derived in
    // any way -- no write occurs, no claim is made, no line is produced, no
    // witness is provided -- and its own pattern, the case outlined above,
    // is merely one among many. These instances were formerly written and
    // printed, a witness `??` was made for the fresh variable of sib1 and
    // b4, and b4's `c q k` was answered as data.
    //
    // Inside the value a firing builds for a collection of the rule's text,
    // the firing retains every variable that it neither binds nor makes a
    // witness for, meaning that any rule within it that holds such a
    // variable is likewise considered half a rule, regardless of whether it
    // contains one condition or several.
    //
    // The choice stems solely from the consequence, before any construction:
    // fresh nodes are made before the consequences are processed, and the
    // instantiation builds the nested `=>` before the fact that holds it,
    // and a `=>` that belongs to nothing is a rule in force (b4's
    // `.list-rules`). Single passes, as counts_over_passes executes them,
    // followed by a run to the fixpoint in the alternate evaluation mode: an
    // engine that makes a witness no fact stands in makes one on every pass.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answered = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };
        const auto printed_deduction = [&]
        {
            return std::any_of(collector.events().begin(), collector.events().end(), [](const auto& e)
                               { return e.channel == zelph::io::OutputChannel::Out && normalize(e.text).find("⇐") != std::string::npos; });
        };
        const auto passes = [&](const std::string& lines)
        {
            interactive.process(".deductions all");
            interactive.process(".semi-naive off");
            interactive.process(".auto-run"); // a toggle: off
            process_lines(interactive, lines);
            const std::size_t before = node_count(collector, interactive);

            std::vector<std::size_t> counts;
            for (int pass = 0; pass < 4; ++pass)
            {
                collector.clear();
                interactive.process(".run-once");
                CHECK_FALSE(printed_deduction());
                counts.push_back(node_count(collector, interactive));
            }
            CHECK(counts == std::vector<std::size_t>(4, before));

            interactive.process(".semi-naive on");
            collector.clear();
            interactive.process(".run");
            CHECK_FALSE(printed_deduction());
            CHECK(node_count(collector, interactive) == before);
        };

        SUBCASE("a fresh variable in the subject (sib1)")
        {
            passes("(X p Y) => (W about ((A r B, A s B) => (c q d)))\na p b");
            CHECK(answered("S about O").empty());
        }
        SUBCASE("a fresh variable of its own beside another consequence")
        {
            // W exists solely in the consequence that is not derived, V in
            // the one that is: the firing makes the witness for V and the
            // fact that names it, and no node for W, which would stand in no
            // fact.
            interactive.process(".deductions all");
            interactive.process(".semi-naive off");
            interactive.process(".auto-run"); // a toggle: off
            process_lines(interactive, "(X p Y) => (W about ((A r B, A s B) => (c q d))) (V t X)\na p b");
            const std::size_t before = node_count(collector, interactive);
            interactive.process(".run-once");
            const std::size_t after = node_count(collector, interactive);
            CHECK(after == before + 2);
            for (int pass = 0; pass < 3; ++pass)
            {
                interactive.process(".run-once");
                CHECK(node_count(collector, interactive) == after);
            }
            CHECK(answered("S t O") == std::vector<std::string>{"?? t a"});
            CHECK(answered("S about O").empty());
        }
        SUBCASE("a bound variable in the subject (sib2)")
        {
            passes("(X p Y) => (X about ((A r B, A s B) => (c q d)))\na p b");
            CHECK(answered("S about O").empty());
        }
        SUBCASE("the outer variables in the mentioned conditions (c1)")
        {
            passes("(X p Y) => (((X q Y, X r Y) => (X s Y)) is noted)\nb p k\nc q d\nc r d");
            CHECK(answered("S is O").empty());
            CHECK(answered("S s O").empty());
        }
        SUBCASE("a predicate variable in the mentioned conditions (b4)")
        {
            passes("(G go H) => (((X G Y, X r Y) => (c q H)) noted W)\nt go k");
            CHECK(answered("S q O").empty());
            CHECK(answered("S noted O").empty());

            // Only the typed rule.
            collector.clear();
            interactive.process(".list-rules");
            CHECK(std::count_if(collector.events().begin(), collector.events().end(), [](const auto& e)
                                { return normalize(e.text).find("=>") != std::string::npos; })
                  == 1);
            CHECK_FALSE(any_output_contains(collector, "(c q k)"));
        }
        SUBCASE("a fresh variable of its own beside a value")
        {
            // Within a value, the firing keeps a variable that it does not
            // bind, thus the rule in that context keeps B. V exists only in
            // the consequence around that value: the firing makes the
            // witness for W and establishes the fact that names it, with no
            // node assigned to V.
            interactive.process(".deductions all");
            interactive.process(".semi-naive off");
            interactive.process(".auto-run"); // a toggle: off
            process_lines(interactive, "(X p Y) => (V about {((X r B) => (c q d))}) (W t X)\na p b");
            const std::size_t before = node_count(collector, interactive);
            interactive.process(".run-once");
            const std::size_t after = node_count(collector, interactive);
            CHECK(after == before + 2);
            for (int pass = 0; pass < 3; ++pass)
            {
                interactive.process(".run-once");
                CHECK(node_count(collector, interactive) == after);
            }
            CHECK(answered("S t O") == std::vector<std::string>{"?? t a"});
            CHECK(answered("S about O").empty());
        }
        SUBCASE("a fresh variable that only a consequence not derived places outside a value")
        {
            // The first consequence is not derived, since its value would
            // keep B, and it is the sole one that holds W outside every
            // container. No witness is made for W, hence the value of the
            // second consequence would keep W, and that one too is not
            // derived. Both were formerly derived, with a witness for W.
            passes("(X p Y) => (W about {((X r B) => (c q d))}) (X has {((W r X) => (c q d))})\na p b");
            CHECK(answered("S about O").empty());
            CHECK(answered("S has O").empty());
        }
        SUBCASE("a witness that the check drops only when it repeats")
        {
            // W stands outside a value only in the first consequence, which is
            // not derived, because its value would keep B. Lacking a witness
            // for W, the value of the second consequence keeps W, meaning that
            // consequence is also not derived, and V, which only the second
            // consequence positions outside a value, needs no witness: the
            // check repeats once a witness has dropped out. Checked only once,
            // the firing made a witness for V that stood in no fact. The single
            // new node is the fact `a t b`.
            interactive.process(".deductions all");
            interactive.process(".semi-naive off");
            interactive.process(".auto-run"); // a toggle: off
            process_lines(interactive, "(X p Y) => (W about {((X r B) => (c q d))}) (V has {((W r X) => (c q d))}) (X t Y)\na p b");
            const std::size_t before = node_count(collector, interactive);
            interactive.process(".run-once");
            const std::size_t after = node_count(collector, interactive);
            CHECK(after == before + 1);
            for (int pass = 0; pass < 3; ++pass)
            {
                interactive.process(".run-once");
                CHECK(node_count(collector, interactive) == after);
            }
            CHECK(answered("S t O") == std::vector<std::string>{"a t b"});
            CHECK(answered("S has O").empty());
            CHECK(answered("S about O").empty());
        }
        SUBCASE("the control: a mention the substitution makes ground (c3)")
        {
            interactive.process(".deductions all");
            interactive.process("(X p Y) => (X about ((X r Y) => (X q Y)))");
            collector.clear();
            interactive.process("k p m");
            CHECK(any_deduction_of(collector, "k about ((k r m) => (k q m))"));
            CHECK(answered("S about O") == std::vector<std::string>{"k about ((k r m) => (k q m))"});
        }
        SUBCASE("the control: a value whose rule the substitution makes ground")
        {
            // Within a value, the firing substitutes the variables its
            // conditions bind and the fresh variable it makes a witness
            // for, so no rule keeps a variable, and data located beside a
            // rule within a value has no effect. The witness identified
            // during the next run is the one created beforehand.
            interactive.process(".deductions all");
            process_lines(interactive, "(X p Y) => (X with {foo ((X r Y) => (X q Y))})\n(X p Y) => (W t X) (X holds {((W r X) => (c q d))})");
            collector.clear();
            interactive.process("k p m");
            CHECK(any_deduction_of(collector, "k with {foo ((k r m) => (k q m))}"));
            CHECK(answered("S with O") == std::vector<std::string>{"k with {foo ((k r m) => (k q m))}"});
            CHECK(answered("S holds O") == std::vector<std::string>{"k holds {((?? r k) => (c q d))}"});

            const std::size_t after = node_count(collector, interactive);
            interactive.process(".run");
            CHECK(node_count(collector, interactive) == after);
        }
        SUBCASE("the control: a relation the firing binds to `in` writes into the rule's collection")
        {
            // A relation that the firing binds to `in` writes into the rule's
            // collection rather than building a value of it, and the term it
            // writes into omits the nested rule, which holds a variable: the
            // membership is derived, both at the top of the consequence and
            // below a fact. Read without a binding, the statement counts such
            // a relation as one that builds a value as well, and the rule
            // inside that value had kept both memberships from being derived.
            // `a p b` binds the relation to `b`: there the firing builds the
            // value, whose nested rule would keep B, and derives nothing.
            interactive.process(".deductions all");
            process_lines(interactive, "(X p R) => (X R {((X s B) => (c q d))})\n(X p R) => ((X R {((X s B, X u B) => (c q d))}) noted yes)");
            collector.clear();
            interactive.process("e p in");
            CHECK(any_deduction_of(collector, "(e in @{e}) noted yes"));
            CHECK(answered("S in O") == std::vector<std::string>{"e in @{e}", "e in @{e}"});

            collector.clear();
            interactive.process("a p b");
            CHECK_FALSE(printed_deduction());
            CHECK(answered("S b O").empty());
            CHECK(answered("S noted O") == std::vector<std::string>{"(e in @{e}) noted yes"});
        } });
}

TEST_CASE("rule patterns: a query in the `=>` position answers no rule whose variables sit in its conditions")
{
    // This query meets the rules themselves as its candidates. Unification
    // rejects rule text based on the closures associated with a candidate's
    // subject and consequence, and the subject of a rule with two conditions
    // is the container that its conditions hang off, which lacks a closure:
    // thus, `S => (g q h)` answered the rule currently in force and `S => (c
    // q d)` answered the mentioned one, both with X and Y unbound, whereas
    // the one-condition twins and zelph/sources answered neither.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y, X r Y) => (g q h)
(X p Y) => (e q f)
((X p Y, X r Y) => (c q d)) is noted
((X p Y) => (c q d)) is noted
)");

        for (const std::string consequence : {"g q h", "e q f", "c q d"})
        {
            INFO(consequence);
            collector.clear();
            interactive.process("S => (" + consequence + ")");
            CHECK(collect_answers(collector).empty());
        }

        collector.clear();
        interactive.process(R"(%(string "SRC-" (string/join (map (fn [c] (string (length (zelph/sources "=>" c)))) [(zelph/fact "g" "q" "h") (zelph/fact "e" "q" "f") (zelph/fact "c" "q" "d")]) ",")))");
        CHECK(any_output_contains(collector, "SRC-0,0,0")); });
}

TEST_CASE("rule patterns: a mentioned rule whose variables sit in its own collection is not data")
{
    // `((a p Y) => (c q d)) is weird` carries the variable from its
    // condition, making the mention a pattern, and a rule over rules does
    // not bind it. Its twin with the variable located within a collection of
    // the rule's own text read as a ground statement: the collection is not
    // a hash, and var_in_closure did not enter it, so `(G => H) => (G seen
    // H)` derived `(a p @{Y}) seen (c q d)` with Y unbound, and `S is O`
    // answered the mention. The collection is entered in the same way that a
    // rule's conditions are -- also where a set constant holds it, also
    // through the walk `.fact-stores off` leaves -- and all three mentions
    // are rule text.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((a p @{Y}) => (c q d)) is weird
((a p Y) => (c q d)) is weird
((a p {@{Y}}) => (c q d)) is odd
(G => H) => (G seen H)
)");
        interactive.run(true, false, false);

        const auto check_no_data = [&]
        {
            for (const std::string query : {"S seen O", "S is O"})
            {
                INFO(query);
                collector.clear();
                interactive.process(query);
                CHECK(collect_answers(collector).empty());
            }
        };
        check_no_data();

        interactive.process(".fact-stores off");
        check_no_data(); });
}

TEST_CASE("rule patterns: a typed rule whose variables sit in its own collection is no data to a rule over rules")
{
    // The typed rule constitutes rule text as well. A rule over rules
    // bound it as data and derived a statement over the typed rule's own
    // node, which then counted as mentioned and stopped firing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p @{Y}) => (c q d)
(G => (c q d)) => ((G => (c q d)) is seen)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S is seen");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p @{Y}) => (c q d)")); });
}

TEST_CASE("rule patterns: a mentioned rule whose variables sit in the collection of a membership is not data")
{
    // The identical scenario where the rule's own collection serves as the
    // object in a membership relation: `a in @{Y}` functions as the
    // condition, `c in @{Y}` acts as the consequence, and the first of them
    // appears among two conditions. The membership is the collection's
    // content and holds a variable solely via its member (refer to the case
    // below), thus all three rules read as ground: `(G => H) => (G seen H)`
    // derived `(a in @{a}) seen (c q d)`, and `S is O` answered every
    // mention. The rule's text is finalized once its `=>` fact is written,
    // and it holds Y through the collection. With the fact stores off from
    // the start, the walk that substitutes for them determines outcomes both
    // at firing and at query.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("fact stores on")
        {
        }
        SUBCASE("fact stores off")
        {
            interactive.process(".fact-stores off");
        }
        process_lines(interactive, R"(
((a in @{Y}) => (c q d)) is weird
((a p b) => (c in @{Y})) is weird
((a in @{Y}, b r c) => (c q d)) is odd
(G => H) => (G seen H)
)");
        interactive.run(true, false, false);

        for (const std::string query : {"S seen O", "S is O"})
        {
            INFO(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

TEST_CASE("rule patterns: a typed rule whose variables sit in the collection of a membership is no data to a rule over rules")
{
    // Its typed twin: the rule over rules bound `(a in @{Y}) => (c q d)` as
    // data, and the statement it derived over the typed rule's node made
    // that rule a mentioned one, which is no longer included in the list
    // and no longer fires.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("fact stores on")
        {
        }
        SUBCASE("fact stores off")
        {
            interactive.process(".fact-stores off");
        }
        process_lines(interactive, R"(
(a in @{Y}) => (c q d)
(G => (c q d)) => ((G => (c q d)) is seen)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S is seen");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a in @{")); });
}

// This test was named "rule patterns: a membership in a rule's own collection
// holds a variable only through its member", and it read the membership via a
// rule using the engine's marking facts, which remained accessible. A mark
// fails to meet any condition because "rule patterns: the mark of a pattern
// answers no query and satisfies no rule". The read surface of zelph/sources
// excludes what holds a variable, as a query would, yet still lists the mark
// of the membership.
TEST_CASE("rule patterns: a membership in a rule's own collection holds a variable only through its member, as the read surface shows")
{
    // A membership is its collection's content. Read through the
    // collection, `c in @{c Y}` holds Y -- or not, based on which of the
    // two memberships the parser wrote first -- and a rule over the
    // engine's marking facts lost the membership that the engine marks as
    // a pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{c Y})
(X ~ K) => (X tagged K)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(R"(%(each s (zelph/sources "~" "rule pattern") (zelph/out (string/format "MARK %q" s))))");
        CHECK(any_output_contains(collector, "MARK <zelph/node c in @{c}>"));
        collector.clear();
        interactive.process("S tagged O");
        CHECK(no_answer_mentions(collector, "rule pattern")); });
}

TEST_CASE("rule patterns: a rule that binds a rule's own collection leaves that rule's text ground")
{
    // A rule over rules binds the collection associated with a ground rule
    // and writes a rule stating X's membership in what it bound. The rule
    // it generates refers to the data term of the collection, ensuring the
    // ground rule keeps its original text, which holds no variable, and
    // remains data to `(H => K) => (H seen K)`. If X were inserted into the
    // ground rule's own collection, the ground rule's text would thereafter
    // contain X, and a rule over rules would no longer see it, or would
    // perceive it only until the text-variable store becomes aware of the
    // write. The walk that stands in for the store following a removal, a
    // load, or `.fact-stores off` reads the identical text, so the answer
    // remains unchanged even after an unrelated `.remove`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        enum class Mode
        {
            EachRun,
            RunTogether,
            StoresOff
        } mode = Mode::EachRun;
        SUBCASE("each rule run as it is written")
        {
            mode = Mode::EachRun;
        }
        SUBCASE("the rules run together")
        {
            mode = Mode::RunTogether;
        }
        SUBCASE("fact stores off")
        {
            mode = Mode::StoresOff;
        }

        struct Shape
        {
            std::string rules;
            std::string seen;
        };
        for (const Shape& shape : {Shape{"(a p b) => (c q @{d})\n(G => (S q C)) => ((X r Y) => (X in C))", "(a p b) seen (c q @{d})"},
                                   Shape{"(a p b) => (c in @{d})\n(G => (S in C)) => ((X q Y) => (X in C))", "(a p b) seen (c in @{d c})"}})
        {
            CAPTURE(shape.rules);
            interactive.process(".new");
            const std::string rules = shape.rules + "\n(H => K) => (H seen K)";
            if (mode == Mode::StoresOff) interactive.process(".fact-stores off");
            if (mode == Mode::RunTogether)
            {
                interactive.process(".auto-run"); // a toggle: off
                process_lines(interactive, rules);
                interactive.run(true, false, false);
                interactive.process(".auto-run"); // on again
            }
            else
                process_lines(interactive, rules);

            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector) == std::vector<std::string>{shape.seen});

            process_lines(interactive, "zz qq zz\n.remove zz");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector) == std::vector<std::string>{shape.seen});
        } });
}

TEST_CASE("rule patterns: a rule that binds a rule's own collection leaves that rule's text ground after .save and .load")
{
    // The same via a round trip, with the walk determining the outcome. The
    // session that wrote the network and the one that loaded it answer
    // identically, and both answer according to what the ground rules' texts
    // say: the rule over rules did not write any member into them. This
    // remains true for two ground rules whose collections contain identical
    // members: each construction names the data term of the collection it
    // bound, and neither leaves the other's text holding a variable.
    const auto file = fs::temp_directory_path() / "zelph_rule_text_bound_test.bin";

    // The answers to `S seen O` after `(H => K) => (H seen K)` has run
    // over the network that `lines` constructs: within the session
    // responsible for its creation, and after that network is saved and
    // loaded into an empty one.
    const auto seen = [&](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& lines)
    {
        const auto answers = [&]
        {
            interactive.process("(H => K) => (H seen K)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S seen O");
            std::vector<std::string> out = collect_answers(collector);
            std::ranges::sort(out);
            return out;
        };

        process_lines(interactive, lines);
        interactive.run(true, false, false);
        interactive.process(".save \"" + file.string() + "\"");
        const std::vector<std::string> in_session = answers();

        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");
        return std::pair{in_session, answers()};
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("a fact over the collection")
        {
            const auto [in_session, after_load] = seen(collector, interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
            CHECK(in_session == std::vector<std::string>{"(a p b) seen (c q @{d})"});
            CHECK(after_load == in_session);
        }
        SUBCASE("a membership in the collection")
        {
            const auto [in_session, after_load] = seen(collector, interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => ((X q Y) => (X in C))
)");
            CHECK(in_session == std::vector<std::string>{"(a p b) seen (c in @{d c})"});
            CHECK(after_load == in_session);
        }
        SUBCASE("two ground rules whose collections hold the same members")
        {
            const auto [in_session, after_load] = seen(collector, interactive, R"(
(a p b) => (c q @{d})
(e p f) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
            CHECK(in_session == std::vector<std::string>{"(a p b) seen (c q @{d})", "(e p f) seen (c q @{d})"});
            CHECK(after_load == in_session);
        } });

    fs::remove(file);
}

TEST_CASE("rule patterns: a rule that binds a rule's conjunction set leaves that rule's conditions as written")
{
    // The rule structure binds the conjunction set of `(a p b, c q d) => (e f
    // g)` as C, and the generated rule states X's membership within C. Where
    // a membership is written, a conjunction set represents its data term;
    // thus, the generated rule writes into that term, while the ground rule
    // keeps its two conditions: it remains data for a rule over rules --
    // within the session, following a load, with the fact stores off -- and
    // continues to fire. If X were included among its conditions, the rule
    // would hold a variable in its conditions, and the generated rule's
    // firing on `k r k` would introduce k as a condition, which no fact could
    // match: the rule would never fire again.
    const auto file = fs::temp_directory_path() / "zelph_conjunction_set_bound_test.bin";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (S f O)) => ((X r Y) => (X in C))
)");
        interactive.run(true, false, false);

        SUBCASE("in the session")
        {
        }
        SUBCASE("after .save and .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".new");
            interactive.process(".load \"" + file.string() + "\"");
        }
        SUBCASE("fact stores off")
        {
            interactive.process(".fact-stores off");
        }

        interactive.process("(H => K) => (H seen K)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"{(c q d) (a p b)} seen (e f g)"});

        process_lines(interactive, "k r k\na p b\nc q d");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S f O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e f g"});
        collector.clear();
        interactive.process("k in O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k in @{k}"}); });

    fs::remove(file);
}

TEST_CASE("rule patterns: a condition set among the conditions is read as well")
{
    // Reasoning::evaluate steps into a condition set that constitutes one
    // of a rule's conditions, so the variables within it belong to the
    // rule. Here, they are its sole variables, situated beneath a ground
    // condition.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((a r b, (C s D, C t D)) => (c q d)) is noted");

        collector.clear();
        interactive.process("S is noted");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a construction's condition set is read as the typed one is")
{
    // A construction writes the rule's conditions as a set literal, and a set
    // whose immediate members are entirely ground qualifies as a set
    // constant, a hash, even if one of those members is a condition set
    // holding the rule's variables. The reading skipped over every hash: the
    // generated rule was treated as ground, and `S => (c q k)` and
    // zelph/sources answered it with C and D unbound, whereas its typed twin,
    // whose conditions the parser writes into a collection, received no
    // response from either. Both rules fire, and both constitute rule text,
    // to the variable store and to the walk `.fact-stores off` leaves.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((a r b, (C G D, C u D)) => (c q H))
t go k
(a r b, (C s D, C u D)) => (c q m)
a r b
x s y
x t y
x u y
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("c q V");
        CHECK(collect_answers(collector).size() == 2);
        CHECK(answers_contain(collector, "c q k"));
        CHECK(answers_contain(collector, "c q m"));

        const auto check_unanswered = [&]
        {
            for (const std::string consequence : {"c q k", "c q m"})
            {
                INFO(consequence);
                collector.clear();
                interactive.process("S => (" + consequence + ")");
                CHECK(collect_answers(collector).empty());
            }
        };
        check_unanswered();

        collector.clear();
        interactive.process(R"(%(string "SRC-" (string/join (map (fn [c] (string (length (zelph/sources "=>" c)))) [(zelph/fact "c" "q" "k") (zelph/fact "c" "q" "m")]) ",")))");
        CHECK(any_output_contains(collector, "SRC-0,0"));

        interactive.process(".fact-stores off");
        check_unanswered(); });
}

TEST_CASE("rule patterns: rule text in its conditions is not data after .save and .load")
{
    // A binary load disarms the variable store, and var_in_closure walks
    // the structure it reads back instead. The walk reads a rule's
    // conditions just as the store does, or the network would answer,
    // following a round trip, what the session that wrote it did not.
    const auto file = fs::temp_directory_path() / "zelph_rule_text_vars_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("((X p Y, X r Y) => (c q d)) is noted");
        interactive.process("x has {((A r B, A s B) => (c q d))}");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S has O");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("S => O");
        CHECK(collect_answers(collector).empty());
    }

    fs::remove(file);
}

TEST_CASE("rule patterns: a mention whose consequence carries a variable stays a pattern")
{
    // The control: in this case, the structural closure reaches a
    // variable via the consequence, in both twins, thus no aspect of the
    // conditions decides it. Neither mention was ever answered.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((A q B, A r B) => (A s B)) is noted
((A q B) => (A s B)) is noted
)");

        collector.clear();
        interactive.process("S is noted");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a rule inside a rule's collection is not data after a firing")
{
    // The rule's collection holds a nested rule whose variables reside
    // within its conditions. The value that a firing builds for the
    // collection would keep them, thus the firing derives nothing
    // ("derived rules: a nested rule a firing writes into a collection or
    // a mention is not answered"), and neither `S likes O` nor `S in O`
    // answers anything -- after the typed rule fired and after a generator
    // that writes the same structure has run. The generator's rule had
    // emerged as a set constant around its partially substituted nested
    // rule, and `S in O` answered that membership.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{((A t B, A r B) => (c q k))})
a p b
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        process_lines(interactive, R"(
(G go H) => ((X p Y) => (X likes @{((A G B, A r B) => (c q H))}))
t go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: .explain does not call a pattern an axiom")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        // Once it is derived, it has a proof like anything else.
        interactive.process("a p b");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "a p b"));
        CHECK_FALSE(any_output_contains(collector, "rule pattern")); });
}

// This test was named "rule patterns: a collection literal in a condition is
// not data", and it read both marks via the query `S ~ "rule pattern"`. A
// mark represents the engine's note about a pattern and does not answer any
// query because "the mark of a pattern answers no query and satisfies no
// rule"; the marks remain present, and the read surface of zelph/sources
// tallies their number.
TEST_CASE("rule patterns: a collection literal in a condition is not data, and the marks of its memberships answer no query")
{
    // A collection literal builds a container plus one PartOf fact per
    // member, and Zelph::collection gives that container a COUNTER id rather
    // than a triple hash -- so mark_rule_patterns dropped it at the is_hash
    // gate and never reached the membership facts. They then read as data:
    // the rule fired on the members of its own literal, which nobody had put
    // there, and .explain called them axioms.
    //
    // A collection has its own identity, so the container a rule writes is
    // one nothing else refers to: there is nothing for such a rule to match
    // and it must derive nothing at all.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in @{a b}) => (X flagged yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(collect_answers(collector).empty());

        // Both membership facts carry the mark, as ordinary graph structure.
        collector.clear();
        interactive.process(R"(%(zelph/out (string "MARKS=" (length (zelph/sources "~" "rule pattern")))))");
        CHECK(any_output_contains(collector, "MARKS=2"));
        collector.clear();
        interactive.process(R"(S ~ "rule pattern")");
        CHECK(collect_answers(collector).empty()); });
}

// A collection that contains a variable is a rule's own text, and the
// membership of its ground member constitutes a pattern bearing a mark of its
// own. That mark responded to `S P O` with `(a in @{a}) ~ "rule pattern"`,
// the collection displayed sans its variable, both beneath a rule applied to
// the collection and beneath a statement that merely assumes the shape of a
// rule.
TEST_CASE("rule patterns: the mark of a member a rule's own collection holds answers no query")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_member_mark.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto written = [&](const std::string& statement)
        {
            interactive.process(statement);
            SUBCASE("in the session") {}
            SUBCASE("after .save and .load")
            {
                interactive.process(".save \"" + file.string() + "\"");
                interactive.process(".load \"" + file.string() + "\"");
            }
        };
        SUBCASE("a rule over the collection") { written("({X a} can nil) => (X ok y)"); }

        collector.clear();
        interactive.process(R"(%(each s (zelph/sources "~" "rule pattern") (zelph/out (string/format "MARK %q" s))))");
        CHECK(any_output_contains(collector, "MARK <zelph/node a in @{a}>"));
        collector.clear();
        interactive.process("S P O");
        CHECK(no_answer_mentions(collector, "rule pattern")); });
    fs::remove(file);
}

// The line below was the second subcase of the preceding case, which pinned
// that the membership it wrote keeps its mark and that the mark answers no
// query. This line takes the form of a rule over a collection, with a bare
// variable as its consequence, so it asks which implications exist, and a
// query's text goes once the query has been answered ("rules: asking which
// implications exist leaves nothing behind"). No membership remains to carry
// a mark, either within the session or after .save and .load.
TEST_CASE("rule patterns: a query in the shape of a rule over a collection leaves no mark")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_query_mark.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process("{X a} => O");
        SUBCASE("in the session") {}
        SUBCASE("after .save and .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
        }

        collector.clear();
        interactive.process(R"(%(zelph/out (string "MARKS=" (length (zelph/sources "~" "rule pattern")))))");
        CHECK(any_output_contains(collector, "MARKS=0"));
        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S P O");
        CHECK(no_answer_mentions(collector, "rule pattern")); });
    fs::remove(file);
}

TEST_CASE("rule patterns: a SET constant in a condition is not a pattern")
{
    // The counterpart, and the reason the two literals had to be told apart.
    // A set constant IS its members -- `a in {a b}` holds by construction,
    // not because anybody claimed it -- so quantifying over them is exactly
    // what the rule means and marking them would be wrong.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes"));

        // A mark answers no query, thus the read surface counts them.
        collector.clear();
        interactive.process(R"(%(zelph/out (string "MARKS=" (length (zelph/sources "~" "rule pattern")))))");
        CHECK(any_output_contains(collector, "MARKS=0")); });
}

// This test was named "rule patterns: a rule's condition set answers no
// query, neither its tag nor its memberships", and it pinned that queries and
// rule conditions skipped the tag `~ conjunction` and the memberships
// associated with a rule's condition set. A rule's text is data, though:
// a rule over rules accesses a rule's conditions via these memberships, and a
// rule can name `~ conjunction` directly, thus both answer again. A
// membership whose member holds a variable constitutes a pattern and answers
// nothing, as previously. What the engine writes regarding a rule beyond its
// text -- the mark of a ground pattern -- still fails to answer any query.
// The rule continues to fire as long as its conditions are satisfied, and a
// rule over memberships that reads it leaves its text alone.
TEST_CASE("rule patterns: a rule's condition set is data, its tag and the memberships of its ground conditions")
{
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_condition_set.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        const auto written = [&](const std::string& rule)
        {
            interactive.process(rule);
            SUBCASE("in the session") {}
            SUBCASE("after .save and .load")
            {
                interactive.process(".save \"" + file.string() + "\"");
                interactive.process(".load \"" + file.string() + "\"");
                interactive.process(".auto-run"); // a load disables it
            }
        };

        std::string              claims;
        std::string              query;
        std::string              derived;
        std::vector<std::string> flagged;
        SUBCASE("conditions with variables")
        {
            written("(A go B, B p C) => (A ok C)");
            claims  = "t go k\nk p m";
            query   = "S ok O";
            derived = "t ok m";
        }
        SUBCASE("ground conditions")
        {
            written("(a p b, c q d) => (e r f)");
            claims  = "a p b\nc q d";
            query   = "S r O";
            derived = "e r f";
            flagged = {"(a p b) flagged yes", "(c q d) flagged yes"};
        }
        SUBCASE("a ground condition beside one with a variable")
        {
            written("(X go Y, a p b) => (X ok Y)");
            claims  = "t go k\na p b";
            query   = "S ok O";
            derived = "t ok k";
            flagged = {"(a p b) flagged yes"};
        }
        SUBCASE("a ground negated condition")
        {
            written("(X go Y, ¬(a r b)) => (X ok Y)");
            claims  = "t go k";
            query   = "S ok O";
            derived = "t ok k";
            flagged = {"((a r b) ~ negation) flagged yes"};
        }
        CAPTURE(derived);

        const std::vector<std::string> tag = answers("S ~ conjunction");
        REQUIRE(tag.size() == 1);
        CHECK(tag.front().ends_with("~ conjunction"));
        CHECK(answers("S in O").size() == flagged.size());
        interactive.process("(M in C) => (M flagged yes)");
        CHECK(answers("S flagged O") == flagged);
        CHECK(answers(R"(S ~ "rule pattern")").empty());

        process_lines(interactive, claims);
        CHECK(answers(query) == std::vector<std::string>{derived});
        CHECK(answers("S ~ conjunction") == tag);
        CHECK(answers("S flagged O") == flagged); });
    fs::remove(file);
}

TEST_CASE("rule patterns: what a rule writes inside a set constant is rule text")
{
    // A set constant's inherent memberships define its essence and stay
    // unmarked. The content the rule inscribed inside it constitutes its
    // text, just as in any other case: `k in m` exists only because of the
    // rule's inscription, and answered a query from the moment it was
    // written. The firing derives the set constant, whose membership
    // constitutes data, and claims what the set holds, much like a statement
    // claims the facts nested in it outside a `=>`: `a likes {(k in m)}`
    // typed performs the same function. Prior to the firing, `k in m` serves
    // as a pattern; following it, it used to stay one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a fact")
        {
            interactive.process("(X q Y) => (X likes {(k in m)})");
            collector.clear();
            interactive.process("S in O");
            CHECK_FALSE(answers_contain(collector, "k in m"));

            interactive.process("a q b");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S in O");
            CHECK(answers_contain(collector, "k in m"));
            CHECK(answers_contain(collector, "(k in m) in {(k in m)}"));
            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "a likes {(k in m)}"));
        }
        SUBCASE("a collection")
        {
            interactive.process("(X q Y) => (X likes {@{c}})");
            collector.clear();
            interactive.process("S in O");
            CHECK_FALSE(answers_contain(collector, "c in @{c}"));
        } });
}

TEST_CASE("rule patterns: a rule a statement mentions writes its collections as rule text")
{
    // `((X p Y) => (X q @{c})) is noted` writes the rule it mentions, and the
    // collection that rule holds is contained within its text, just as in a
    // typed rule: its membership answers no query. The remaining ground parts
    // of the mention are also patterns; the mentioned rule itself remains
    // unmarked, because it can function as data and as a premise ("explain: a
    // ground rule a statement talks about is a premise").
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((X p Y) => (X q @{c})) is noted");
        collector.clear();
        interactive.process("S in O");
        CHECK_FALSE(any_output_starts_with(collector, "Answer:"));

        interactive.process("((a p b) => (c q d)) is odd");
        collector.clear();
        interactive.process("S p O");
        CHECK_FALSE(answers_contain(collector, "a p b")); });
}

TEST_CASE("rule patterns: a container the rule did not build keeps its facts")
{
    // The control for the walk's `fresh` gate. A set the rule merely REFERS
    // to -- here by the name `s`, which the data made a container -- is not
    // this construction's doing, so its membership facts stay data and the
    // rule fires on them. Red if the container walk marked whatever it found.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x in s
a in s
(X in s) => (X flagged yes)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "x flagged yes"));
        CHECK(answers_contain(collector, "a flagged yes"));

        // A mark answers no query, thus the read surface counts them.
        collector.clear();
        interactive.process(R"(%(zelph/out (string "MARKS=" (length (zelph/sources "~" "rule pattern")))))");
        CHECK(any_output_contains(collector, "MARKS=0")); });
}

TEST_CASE("rule patterns: the mark survives .save and .load")
{
    // The record is a fact in the graph, not something the session
    // remembers -- so a round trip has to keep it, and the in-memory index
    // has to be read back off the graph afterwards.
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("(a p b) => (c q d)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        // .load disables auto-run.
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));
    }

    fs::remove(file);
}

TEST_CASE("rule patterns: the marks of a mention survive .save and .load")
{
    // The marks a mention writes constitute facts within the graph, just as
    // those of a typed rule do. Following the load, claiming the condition
    // does not put the mentioned rule in force, thus its consequence stays a
    // pattern.
    const auto file = fs::temp_directory_path() / "zelph_mention_pattern_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("((a p b) => (c q d)) is noted");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"((a p b) => (c q d)) is noted"});

        // .load disables auto-run.
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector).empty());
    }

    fs::remove(file);
}

TEST_CASE("rule patterns: the marks of a query survive .save and .load")
{
    // The marks the query writes under its operators are facts in the graph,
    // just as a rule's are: following a round trip, the operand still
    // answers nothing.
    const auto file = fs::temp_directory_path() / "zelph_query_pattern_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("t go k");
        interactive.process("X go Y, ¬(a p b)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
    }

    fs::remove(file);
}

// This test was named "rule patterns: a composite predicate in a pattern is
// not data either", and it read both marks via the query `S Q O`. A mark
// does not answer any query because "rule patterns: the mark of a pattern
// answers no query and satisfies no rule", hence the marks are extracted
// from the extent of their predicate.
TEST_CASE("rule patterns: a composite predicate in a pattern is not data either, and carries the mark")
{
    // The descent through a rule's patterns walked the subject and the
    // objects but not the PREDICATE. For every ordinary rule that costs
    // nothing -- a named atom has no fact structure to descend into -- but a
    // COMPOSITE predicate is a ground fact node like any other, and writing
    // the rule is what brought it into being:
    //
    //     (a (b r s) c) => (d q e)
    //
    // left `b r s` behind as data, so `(X r Y) => (X leaked Y)` derived
    // `b leaked s` from a fact nobody had claimed. Exactly the leak afc0f3e
    // closed for the other two positions.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a (b r s) c) => (d q2 e)
(X r Y) => (X leaked Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S leaked O");
        CHECK(collect_answers(collector).empty());

        // It carries the marker, like the condition and the consequence.
        collector.clear();
        interactive.process(R"(%(each s (zelph/sources "~" "rule pattern") (zelph/out (string/format "MARK %q" s))))");
        CHECK(any_output_contains(collector, "MARK <zelph/node b r s>"));
        CHECK(any_output_contains(collector, "MARK <zelph/node a (b r s) c>"));
        collector.clear();
        interactive.process("S Q O");
        CHECK(no_answer_mentions(collector, "rule pattern")); });
}

TEST_CASE("rule patterns: asserting a composite predicate revokes its mark too")
{
    // The counterpart: once the statement is CLAIMED it is data, and the rule
    // that quantifies over it fires -- the mark is revoked exactly as it is
    // for a subject or an object pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a (b r s) c) => (d q2 e)
b r s
(X r Y) => (X leaked Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S leaked O");
        CHECK(answers_contain(collector, "b leaked s")); });
}
