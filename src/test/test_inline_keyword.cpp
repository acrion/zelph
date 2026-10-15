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
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Inline keywords ("expression islands"): the 3-argument form of
// zelph/register-keyword. An island (open ... close) inside a zelph
// statement is passed to its Janet handler, whose zelph/node result is
// spliced back into the statement via the unquote mechanism. This is the
// host mechanism behind stdlib term islands like $( ... ): the island
// GRAMMAR lives in scripts, only the splice lives in C++.
// ---------------------------------------------------------------------------

namespace
{
    // Minimal island: resolve the trimmed content as a node name.
    constexpr const char* kAtomIsland =
        R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/resolve (string/trim t)))))js";

    // Paren-balanced island: vetoes with :incomplete until the content is
    // balanced -- the pattern real island grammars use. Side-effect-free
    // before acceptance, as the contract requires.
    constexpr const char* kBalancedIsland =
        R"js(%(zelph/register-keyword "$(" ")" (fn [t] (var d 0) (each c t (case c 40 (++ d) 41 (-- d))) (if (= d 0) (zelph/resolve (string/trim t)) :incomplete))))js";

    // An island that builds structure, in the manner a term island does:
    // three names make the fact of them, and anything else is an error of
    // the handler.
    constexpr const char* kFactIsland =
        R"js(%(zelph/register-keyword "$(" ")" (fn [t] (def w (string/split " " (string/trim t))) (if (= (length w) 3) (zelph/fact (w 0) (w 1) (w 2)) (error "three names")))))js";
} // namespace

TEST_CASE("inline keyword: islands splice into value positions")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kAtomIsland);

        SUBCASE("subject and object of a top-level fact")
        {
            interactive.process(R"($( berlin ) "is capital of" $( germany ))");
            collector.clear();
            interactive.process(R"js(%(string "IK-SPO-" (zelph/exists "berlin" "is capital of" "germany")))js");
            CHECK(any_output_contains(collector, "IK-SPO-true"));
        }
        SUBCASE("inside a nested fact")
        {
            interactive.process(R"((tom saw $( sunrise )) ~ verified)");
            collector.clear();
            interactive.process(R"js(%(string "IK-NEST-" (zelph/exists (zelph/fact "tom" "saw" "sunrise") "~" "verified")))js");
            CHECK(any_output_contains(collector, "IK-NEST-true"));
        }
        SUBCASE("a lone island is a complete statement and echoes its node")
        {
            collector.clear();
            interactive.process("$( solo )");
            CHECK(any_output_contains(collector, "solo"));
        } });
}

TEST_CASE("inline keyword: veto protocol extends the island across nested close delimiters")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kBalancedIsland);
        interactive.process(R"($( a(b)c ) ~ tricky)");
        collector.clear();
        // The handler saw the FULL balanced content in one piece; the
        // truncated first split (" a(b") must never have been resolved --
        // the veto happens before any side effect.
        interactive.process(R"js(%(string "IK-VETO-" (zelph/exists "a(b)c" "~" "tricky")))js");
        interactive.process(R"js(%(string "IK-VETO-NOT-" (zelph/exists "a(b" "~" "tricky")))js");
        CHECK(any_output_contains(collector, "IK-VETO-true"));
        CHECK(any_output_contains(collector, "IK-VETO-NOT-false")); });
}

TEST_CASE("inline keyword: island handlers share the statement's variable scope")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The island builds the rule CONDITION pattern (A likes <content>);
        // the consequence outside the island reuses A. Both must bind the
        // SAME variable node, otherwise the rule never fires.
        interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/fact 'A "likes" (zelph/resolve (string/trim t))))))js");
        interactive.process(R"($( beer ) => (A celebrates A))");
        interactive.process("tom likes beer");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(string "IK-VAR-" (zelph/exists "tom" "celebrates" "tom")))js");
        CHECK(any_output_contains(collector, "IK-VAR-true")); });
}

TEST_CASE("block keyword: a handler call is one variable scope")
{
    // A pin of documented behaviour (janet.md, Extending the REPL): each
    // invocation of a handler corresponds to a single evaluation of a Janet
    // block, meaning a variable symbol refers to a single variable across the
    // whole call, in every zelph/fact it makes, and conditions constructed
    // through distinct invocations of zelph/fact join on it. A zelph/query
    // starts a new scope, just as it does within a `%` block, and the next
    // call of the handler starts with fresh variables.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"js(%(def kv-calls @[])
%(zelph/register-keyword "kv" (fn [text]
  (zelph/fact "a" "p" "b") (zelph/fact "a" "q" "c") (zelph/fact "z" "p" "b")
  (def f1 (zelph/fact 'X "p" "b"))
  (def f2 (zelph/fact 'X "p" "b"))
  (def g (zelph/fact 'X "q" "c"))
  (def cs (zelph/set f1 g))
  (zelph/fact cs "~" "conjunction")
  (def answers (map (fn [r] (zelph/name (get r 'X))) (zelph/query cs)))
  (def g2 (zelph/fact 'X "q" "c"))
  (array/push kv-calls {:same (= f1 f2) :answers answers :after-query (= g g2) :f1 f1})
  nil)))js");

        interactive.process("kv first");
        interactive.process("");
        interactive.process("kv second");
        interactive.process("");
        REQUIRE(janet_value(collector, interactive, "(length kv-calls)") == "2");

        // The two facts constructed using 'X are a single pattern,
        // and the conjunction applied to the two conditions answers `a`
        // exclusively, rather than the cross product.
        CHECK(janet_value(collector, interactive, "(let [c (kv-calls 0)] [(c :same) (c :answers)])") == R"((true @["a"]))");
        CHECK(janet_value(collector, interactive, "((kv-calls 0) :after-query)") == "false");
        CHECK(janet_value(collector, interactive, "(= ((kv-calls 0) :f1) ((kv-calls 1) :f1))") == "false"); });
}

// janet.md said that the scope of a handler call lasts until a `zelph/query`
// starts a new one. A `zelph/import` within the call also begins a new scope,
// because every line it processes constitutes an independent block, and the
// original sentence omitted this detail. The behaviour remains unchanged, and
// janet.md now explicitly identifies it: this pins it, ensuring any
// modification to it is deliberate. A `zelph/run-script` and a `zelph/run`
// keep the scope. The import of a file previously imported by the session
// reads no line and keeps the scope as well.
TEST_CASE("block keyword: a zelph/import inside a handler call starts a new scope, a zelph/run-script does not")
{
    namespace fs      = std::filesystem;
    const auto module = fs::temp_directory_path() / "zelph_handler_scope_inner.zph";
    const auto script = fs::temp_directory_path() / "zelph_handler_scope_script.janet";
    {
        std::ofstream(module) << "c q d\n";
        std::ofstream(script) << "(zelph/fact \"u\" \"v\" \"w\")\n";
    }
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const auto                  same_after = [&](const std::string& call)
    {
        interactive.process("%(def kv-same @[])");
        interactive.process(R"js(%(zelph/register-keyword "kv" (fn [text] (def f1 (zelph/fact 'X "p" "b")) )js" + call
                            + R"js( (array/push kv-same (= f1 (zelph/fact 'X "p" "b"))) nil)))js");
        interactive.process("kv x");
        interactive.process("");
        return janet_value(collector, interactive, "kv-same");
    };

    SUBCASE("zelph/import")
    {
        CHECK(same_after("(zelph/import \"" + module.generic_string() + "\")") == "@[false]");
        CHECK(same_after("(zelph/import \"" + module.generic_string() + "\")") == "@[true]");
    }
    SUBCASE("zelph/run-script") { CHECK(same_after("(zelph/run-script \"" + script.generic_string() + "\")") == "@[true]"); }
    SUBCASE("zelph/run") { CHECK(same_after("(zelph/run)") == "@[true]"); }

    fs::remove(module);
    fs::remove(script);
}

TEST_CASE("block keyword: a quoted atom escapes the keyword")
{
    // A pin of documented behaviour (janet.md, Extending the REPL): a block
    // keyword shadows any statement whose first token matches the bare
    // atom exactly, and enclosing the atom in quotes escapes it. That a
    // node with the same name as the keyword prints quoted is pinned in
    // test_node_display.cpp; what is pinned here is that the unquoted form
    // arrives at the handler and makes no assertion.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(def kw-seen @[]))js");
        interactive.process(R"js(%(zelph/register-keyword "kw" (fn [t] (array/push kw-seen t) nil)))js");

        interactive.process("\"kw\" ~ card");
        interactive.process("kw ~ card");
        interactive.process("");

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "SEEN-" (length kw-seen) "-" (string/trim (kw-seen 0)))))js");
        CHECK(last_out_text(collector) == "SEEN-1-~ card");

        collector.clear();
        interactive.process("X ~ card");
        CHECK(collect_answers(collector) == std::vector<std::string>{"\"kw\" ~ card"}); });
}

// A handler constitutes Janet code authored by the user, much like a `%` block
// does, and the raw `=>` facts it generates are validated upon return. A rule
// in force whose consequence predicate no condition binds becomes the error of
// the block the handler answered, before the handler's output is displayed.
TEST_CASE("block keyword: a raw `=>` the handler writes is checked when the handler returns")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(zelph/register-keyword "rk" (fn [text] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'W "nil")) "handler done")))js");
        interactive.process("rk anything");
        collector.clear();
        CHECK_THROWS_WITH(interactive.process(""), doctest::Contains("Error in line \"rk anything\": zelph/fact: The predicate of \"X W nil\""));
        CHECK_FALSE(any_output_contains(collector, "handler done"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// An island executes during the parsing of its line, before any code within
// the statement being processed, meaning the recording of a rule build never
// saw what the island built: "($( a p b ) q X) => (X r s)" left `a p b` as
// data, which answered "A p B" and fed every rule over it, whereas the
// identical rule written without the island maintains it as a pattern. What
// the islands of a statement have built now constitutes what the statement
// has built, and the rule marks what it reaches of that construction.
TEST_CASE("inline keyword: a ground term an island builds inside a rule is a pattern of the rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kFactIsland);

        SUBCASE("a typed rule") { interactive.process("($( a p b ) q X) => (X r s)"); }
        SUBCASE("a rule a line mentions") { interactive.process("x about (($( a p b ) q X) => (X r s))"); }

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]")); });
}

// The same principle applies under a condition operator within a query:
// the structure the query builds there constitutes a pattern
// (zelph/query-text), and a term an island built for it was left as data. A
// term that the query also uses outside the operator is created there, and
// thereby asserted, just as any part of a query is.
TEST_CASE("inline keyword: a ground term an island builds beneath ¬ in a query is a pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kFactIsland);
        interactive.process("t go k");

        collector.clear();
        interactive.process("X go Y, ¬(X q $( a p b ))");
        CHECK(collect_answers(collector).size() == 1);
        interactive.process("X go Y, $( c p d ) s Y, ¬(X r $( c p d ))");

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c p d"}); });
}

// The parser makes a judgment on a line once its islands have completed
// execution. A line that gets rejected was never accepted, yet the
// constructions its islands produced stayed: following the rejection of "$(
// a p b ) q c, ¬(d q e)" due to its operator, "A p B" answered `a p b`, and
// the arithmetic rules from the standard library fired on a term that no
// accepted statement contained. A line that fails to parse entirely is
// rejected in the same way.
TEST_CASE("inline keyword: a line the parser refuses keeps nothing its island built")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kFactIsland);
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process("$( a p b ) q c, ¬(d q e)"), doctest::Contains("condition operator"));
        CHECK_THROWS_WITH(interactive.process("$( a p b ) q x(c d e)"), doctest::Contains("Could not parse"));

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty()); });
}

// A handler's error ends the line before the parser sees it, and the islands
// located to its left had already completed their construction as requested.
TEST_CASE("inline keyword: an island that fails leaves nothing the islands before it built")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kFactIsland);
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process("$( a p b ) q $( fail )"), doctest::Contains("three names"));

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty()); });
}

// An island operates during the parsing of its line, before the execution of
// the statement's code, and the statement may hold what the island returns.
// Thus, the raw `=>` facts that an island writes are checked when the
// statement ends: a rule that the statement holds is rule text that passes,
// whereas a rule that the island produces alongside the node it returns is a
// rule in force, and is refused as the error of the line.
TEST_CASE("inline keyword: a raw `=>` an island writes is checked at the end of its statement")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the statement holds it")
        {
            interactive.process(R"js(%(zelph/register-keyword "$<" ">" (fn [t] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'V "nil")))))js");
            collector.clear();
            CHECK_NOTHROW(interactive.process("x says $<abc>"));
            CHECK(any_output_contains(collector, "x says ((X waits nil) => (X V nil))"));
        }
        SUBCASE("the island returns another node")
        {
            interactive.process(R"js(%(zelph/register-keyword "$<" ">" (fn [t] (zelph/fact (zelph/fact 'X "waits" "nil") "=>" (zelph/fact 'X 'V "nil")) (zelph/fact "abc" "is" "text"))))js");
            CHECK_THROWS_WITH(interactive.process("x says $<abc>"), doctest::Contains("Error in line \"x says $<abc>\": zelph/fact: The predicate of \"X V nil\""));
        }

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// A handler may parse statements of its own: as the island executes, a file
// it imports gets parsed one line at a time. Each of those parses ended the
// line's recording as though it were its own, meaning what the handler built
// after the import was recorded by nobody, and the term the island returned
// remained data within the rule. Only the parse that opened the recording
// ends it.
TEST_CASE("inline keyword: a handler that imports a file still records what its island builds")
{
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_island_import.zph";
    {
        std::ofstream f(script);
        f << "m n o\n";
    }
    const std::string handler = R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/import ")js" + script.generic_string()
                              + R"js(") (zelph/fact "a" "b" "c"))))js";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(handler);
        interactive.process("($( x ) q X) => (X r s)");

        collector.clear();
        interactive.process("A b C");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("M n O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"m n o"}); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("inline keyword: no expansion inside quoted atoms or comments")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(var ik-calls 0))js");
        interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] (++ ik-calls) (zelph/resolve (string/trim t)))))js");

        interactive.process(R"js("prefix $( not an island )" ~ quoted)js");
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "IK-CALLS-A-" ik-calls)))js");
        CHECK(any_output_contains(collector, "IK-CALLS-A-0"));

        interactive.process(R"($( real ) ~ island)");
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "IK-CALLS-B-" ik-calls)))js");
        interactive.process(R"js(%(string "IK-REAL-" (zelph/exists "real" "~" "island")))js");
        CHECK(any_output_contains(collector, "IK-CALLS-B-1"));
        CHECK(any_output_contains(collector, "IK-REAL-true"));

        // An island located within a trailing comment is considered part of
        // that comment; a '#' found inside a name does not initiate a new
        // comment, thus the island following it is counted.
        interactive.process(R"(x p y   # $( not an island ))");
        interactive.process(R"(x p y#z $( counted ))");
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "IK-CALLS-C-" ik-calls)))js");
        CHECK(any_output_contains(collector, "IK-CALLS-C-2")); });
}

TEST_CASE("inline keyword: registration and use from an imported script")
{
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_inline_island.zph";
    {
        std::ofstream f(script);
        f << R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/resolve (string/trim t)))))js" << "\n";
        f << R"($( imported-a ) linked $( imported-b ))" << "\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".import \"" + script.string() + "\"");
        collector.clear();
        interactive.process(R"js(%(string "IK-IMP-" (zelph/exists "imported-a" "linked" "imported-b")))js");
        CHECK(any_output_contains(collector, "IK-IMP-true"));

        // The registration persists after the import.
        interactive.process(R"($( post ) linked $( hoc ))");
        collector.clear();
        interactive.process(R"js(%(string "IK-POST-" (zelph/exists "post" "linked" "hoc")))js");
        CHECK(any_output_contains(collector, "IK-POST-true")); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("inline keyword: error cases")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("handler that never accepts")
        {
            interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] :incomplete)))js");
            CHECK_THROWS_AS(interactive.process("$( x ) ~ y"), std::runtime_error);
        }
        SUBCASE("missing closing delimiter")
        {
            interactive.process(R"js(%(zelph/register-keyword "$[" "]" (fn [t] (zelph/resolve t))))js");
            CHECK_THROWS_AS(interactive.process("$[ x ~ y"), std::runtime_error);
        }
        SUBCASE("handler result must be a zelph/node")
        {
            interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] "not-a-node")))js");
            CHECK_THROWS_AS(interactive.process("$( x ) ~ y"), std::runtime_error);
        } });
}

TEST_CASE("block keyword: a handler that imports a script reads the script, not its own block")
{
    // A blank line dispatched the handler while the keyword state was still
    // collecting, causing the lines from a file imported by the handler to
    // be placed into the keyword buffer: none of these lines were read, and
    // the end of the import dispatched the handler a second time, with
    // those lines appended. A file consisting of a single chunk never had
    // that chunk run; a file with two chunks lost its first chunk. The end
    // of input was never affected by this issue, since finish_input leaves
    // the state before the handler runs.
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_keyword_import.zph";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        std::vector<std::string> expected;
        SUBCASE("a script of two chunks")
        {
            std::ofstream f(script);
            f << "alpha rel beta\n\ngamma rel delta\n";
            expected = {"alpha rel beta", "gamma rel delta"};
        }
        SUBCASE("a one-chunk file runs its chunk")
        {
            std::ofstream f(script);
            f << "alpha rel beta\n";
            expected = {"alpha rel beta"};
        }

        interactive.process("%(def kw-calls @[])");
        interactive.process(R"js(%(zelph/register-keyword "kl" (fn [t] (array/push kw-calls t) (zelph/import ")js" + script.generic_string() + R"js(") nil)))js");
        interactive.process("kl first");
        interactive.process("");

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "CALLS-" (length kw-calls))))js");
        CHECK(any_output_contains(collector, "CALLS-1"));

        collector.clear();
        interactive.process("X rel Y");
        std::vector<std::string> answers = collect_answers(collector);
        std::sort(answers.begin(), answers.end());
        CHECK(answers == expected); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("block keyword: a handler that ran after its last write is not followed by another run")
{
    // Following every block that a blank line dispatched, the host ran the
    // engine anew, despite the handler having just run it. That run derived
    // nothing, as the handler had left a fixpoint, yet it printed each
    // contradiction without a record once again, in zelph syntax, right
    // after a handler that had received it as data from a quiet run. No
    // record is maintained for the contradiction (records off), thus every
    // run encountering it prints its line: the line's absence after a block
    // indicates no subsequent run followed, while its presence signals that
    // one did.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".contradiction-records off\n(X waits nil) => !\n(X ~ dog) => (X ~ animal)\njohn waits nil\nbob ~ cat");
        const auto ran_after_block = [&] { return any_event_contains(collector, "⇐ (john waits nil)"); };
        const auto animal          = [&]
        {
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "ANIMAL-" (zelph/exists "bob" "~" "animal"))))js");
            return any_output_contains(collector, "ANIMAL-true");
        };

        SUBCASE("the blank line that dispatches it")
        {
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/fact "bob" "~" "dog") (zelph/run :quiet) nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK_FALSE(ran_after_block());
            CHECK_FALSE(has_contradiction(collector));
            CHECK(animal());
        }
        SUBCASE("the end of input skips it the same way")
        {
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/fact "bob" "~" "dog") (zelph/run :quiet) nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.finish_input();
            CHECK_FALSE(ran_after_block());
            CHECK(animal());
        }
        SUBCASE("a question cluster dropped after the run is no write")
        {
            // The query writes its pattern within the cluster, and the drop
            // removes it again from the record of additions (P-drop-delta),
            // enabling a handler to ask a question after its run.
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/fact "bob" "~" "dog") (zelph/run :quiet) (def prev (zelph/cluster)) (zelph/cluster "q") (zelph/out (string "ROWS-" (length (zelph/query (zelph/fact 'A "~" "animal"))))) (zelph/cluster-drop "q") (zelph/cluster prev) nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(any_output_contains(collector, "ROWS-1"));
            CHECK_FALSE(ran_after_block());
        }
        SUBCASE("a write after the handler's run is followed by the host's run")
        {
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/run :quiet) (zelph/fact "bob" "~" "dog") nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
            CHECK(animal());
        }
        SUBCASE("in classic evaluation a write after the handler's run is followed by the host's run")
        {
            // Classic evaluation does not seed from the record of additions,
            // but the skip still reads it in every mode: a write occurring
            // after the handler's run must still store its entry there in
            // classic evaluation as well.
            interactive.process(".semi-naive off");
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/run :quiet) (zelph/fact "bob" "~" "dog") nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
            CHECK(animal());
        }
        SUBCASE("a handler that did not run is followed by the host's run, even when it wrote nothing")
        {
            // Pinned since the graph constitutes a fixpoint in this context
            // too: solely the handler's own run conveys to the host that the
            // handler has seen it.
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
        }
        SUBCASE("a single pass that derived something is followed by the host's run")
        {
            // A .run-once that derived something did not determine whether an
            // additional pass would derive more, thus it has not left a
            // fixpoint.
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/fact "bob" "~" "dog") (zelph/run-once :quiet) nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
        }
        SUBCASE("a fact removed after the run is followed by the host's run")
        {
            // The cluster holds a single fact about older nodes, causing the
            // drop to remove that fact alone; it held at the run's end, and
            // with its removal the graph is no longer known to be a fixpoint.
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (def prev (zelph/cluster)) (zelph/cluster "c") (zelph/fact "bob" "~" "dog") (zelph/cluster prev) (zelph/run :quiet) (zelph/cluster-drop "c") nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
        } });
}

TEST_CASE("block keyword: a handler whose run left the negation records untested is followed by the host's run")
{
    // A complete run re-tests the negation records, just as the host's run
    // does following a block. If the handler's own run had not re-tested
    // them, skipping the host's run shifted the report to the run
    // immediately after the next input line: a warning appeared after a line
    // unrelated to it, and in check mode that line raised the negation check
    // as its own error, while piped input ceased at that point. .semi-naive
    // on with .negation-records on keeps the records without check mode's
    // error, so the warning makes clear which run reports the defeat.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".semi-naive on\n.negation-records on\n(M ~ man, ¬(M h1 nil)) => (M is happy)\nbill ~ man\n.contradiction-records off\n(X waits nil) => !\njohn waits nil");
        const auto warned          = [&] { return any_output_contains(collector, "bill is happy  (by"); };
        const auto ran_after_block = [&] { return any_event_contains(collector, "⇐ (john waits nil)"); };

        SUBCASE("a single pass never re-tests them")
        {
            // The pass derives nothing, thus the graph is a fixpoint, yet a
            // single pass never re-tests the records.
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/fact "bill" "h1" "nil") (zelph/run-once :quiet) nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
            CHECK(warned());
        }
        SUBCASE("a drop after the run gives a reported record back")
        {
            // A run within the cluster old reported the defeat, and the drop
            // returns the record. The defeating fact was written before the
            // cluster, which holds no node: the drop removes nothing, thus
            // the graph remains the fixpoint of the handler's run, though the
            // record waits for a re-test.
            process_lines(interactive, ".auto-run\nbill h1 nil\n.cluster old");
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "OLD-" (length ((zelph/run :quiet) :naf-final)))))js");
            REQUIRE(any_output_contains(collector, "OLD-1"));
            process_lines(interactive, ".cluster default\n.auto-run");
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (zelph/fact "carl" "~" "dog") (zelph/run :quiet) (zelph/cluster-drop "old") nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(ran_after_block());
            CHECK(warned());
        }
        SUBCASE("a dry run dropped before the handler's last run keeps the skip")
        {
            // The shape a front end imposes on a block: a dry run within a
            // scratch cluster reports the defeat, the drop gives the record
            // back, and the subsequent run after the real write reports it
            // once more. Since the drop occurred before that run, the graph
            // constitutes the fixpoint of that run, encompassing its records
            // as well.
            interactive.process(R"js(%(zelph/register-keyword "kr" (fn [t] (def prev (zelph/cluster)) (zelph/cluster "dry") (zelph/fact "bill" "h1" "nil") (def d ((zelph/run :quiet) :naf-final)) (zelph/cluster prev) (zelph/cluster-drop "dry") (zelph/fact "bill" "h1" "nil") (def r ((zelph/run :quiet) :naf-final)) (zelph/out (string "DRY-" (length d) " REAL-" (length r))) nil)))js");
            interactive.process("kr go");
            collector.clear();
            interactive.process("");
            CHECK(any_output_contains(collector, "DRY-1 REAL-1"));
            CHECK_FALSE(ran_after_block());
            CHECK_FALSE(warned());
        } });
}

TEST_CASE("input end: a half-read block is dispatched, not discarded")
{
    // process_file flushed an unterminated keyword block, multi-line
    // statement or Janet block at the end of a script; the REPL loop did
    // not, so the same input ran from a FILE and vanished from stdin. That
    // is the shape of every "printf ... | zelph" verification, which made a
    // query that never ran indistinguishable from one that found nothing.
    //
    // Interactive::finish_input is what both paths call now; here it stands
    // in for reaching the end of the input.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a multi-line statement completes")
        {
            interactive.process("a rel");
            interactive.process("b");
            interactive.finish_input();

            collector.clear();
            interactive.process("a rel X");
            CHECK(answers_contain(collector, "a rel b"));
        }
        SUBCASE("a Janet block runs")
        {
            interactive.process("%");
            interactive.process("(zelph/fact \"j\" \"ran\" \"yes\")");
            interactive.finish_input();

            collector.clear();
            interactive.process("j ran X");
            CHECK(answers_contain(collector, "j ran yes"));
        }
        SUBCASE("a keyword block dispatched by the end of input is followed by a run")
        {
            // A blank line that dispatches a block is followed by the
            // auto-run; the end of input dispatched the identical block
            // and ran nothing, meaning the handler's assertion derived
            // nothing until the next input line, which piped input never
            // has.
            interactive.process(R"js(%(zelph/register-keyword "kf" (fn [t] (zelph/fact "j" "is" "happy") nil)))js");
            interactive.process("(X is happy) => (X is glad)");
            interactive.process("kf");
            collector.clear();
            interactive.finish_input();
            CHECK(any_deduction_of(collector, "j is glad"));
        }
        SUBCASE("an unterminated Janet block dispatched by the end of input is followed by a run")
        {
            interactive.process("(X is happy) => (X is glad)");
            interactive.process("%");
            interactive.process("(zelph/fact \"j\" \"is\" \"happy\")");
            collector.clear();
            interactive.finish_input();
            CHECK(any_deduction_of(collector, "j is glad"));
        }
        SUBCASE("an unfinished statement is reported")
        {
            interactive.process("(a rel");
            CHECK_THROWS_WITH_AS(interactive.finish_input(),
                                 doctest::Contains("unfinished statement"),
                                 std::runtime_error);
        }
        SUBCASE("a statement missing its object says so, in zelph's own terms")
        {
            // "a p" -- the commonest typo there is -- accumulates like any
            // other unfinished line, but the PEG can still make a
            // two-argument zelph/fact of it. At end of input that was run
            // anyway, and the user was handed Janet's complaint about an
            // internal call: "arity mismatch, expected at least 3, got 2".
            interactive.process("a rel");
            CHECK_THROWS_WITH_AS(interactive.finish_input(),
                                 doctest::Contains("unfinished statement"),
                                 std::runtime_error);
        }
        SUBCASE("a parenthesised statement is a term, and says so at end of input")
        {
            // "(a rel b)" is how the renderer prints a nested fact and how
            // .explain takes its argument, so typing it as a statement is a
            // plausible slip. It is a TERM, hence a statement PREFIX, and
            // waits for the rest.
            interactive.process("(a rel b)");
            CHECK_THROWS_WITH_AS(interactive.finish_input(),
                                 doctest::Contains("unfinished statement"),
                                 std::runtime_error);
        }
        SUBCASE("a command inside an unfinished statement says which one")
        {
            // The buffer survives a command, and the NEXT statement line is
            // appended to it -- "(a rel b)" followed by "c q d" asserts
            // "(a rel b) c q d", and neither line does what it says. The
            // behaviour is the documented multi-line rule; what was missing
            // is any sign of the state the command runs in.
            interactive.process("(a rel b)");
            collector.clear();
            interactive.process(".stat");
            CHECK(any_event_contains(collector, "still inside an unfinished statement"));
            CHECK(any_event_contains(collector, "(a rel b)"));

            // A command outside one stays quiet.
            interactive.process("c q d");
            collector.clear();
            interactive.process(".stat");
            CHECK_FALSE(any_event_contains(collector, "still inside an unfinished statement"));
        }
        SUBCASE("finishing twice is harmless")
        {
            interactive.process("a rel b");
            interactive.finish_input();
            interactive.finish_input();
        } });
}
