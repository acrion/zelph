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

#include "string/string_utils.hpp"
#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// A script named on the command line is a SESSION, not a library load.
//
// `zelph script.zph` used to run the file through the same path as `.import`,
// which suppresses three things a session has: the input echo, the collection
// of focus anchors, and auto-run after each line. Each of those cost the file
// form something its piped twin `zelph < script.zph` had, and none of it was
// visible from the outside:
//
//   - no echo, and the echo is what records the node a bare `.node`,
//     `.mermaid` or `.explain` falls back to. So the file form answered
//     "No argument given and no previous output node available" -- while the
//     shebang `#!/usr/bin/env zelph`, which every zelph script carries,
//     points at exactly that form;
//   - no focus anchors, so the default deduction filter had nothing to keep
//     and removed EVERY deduction. `zelph script.zph > log` wrote an EMPTY
//     file for a script whose piped twin printed its whole derivation, and
//     the notice saying so went to stderr;
//   - auto-run deferred to the end of the file, so a script could not read a
//     result it had just derived.
//
// What a session keeps from a module is everything about finding and
// preparing the file: standard-library resolution, the `.janet` runner,
// script arguments, the import-once module guard. See ScriptRole in
// src/lib/repl_state.hpp.
//
// The second subject here is what a LINE is. zelph decides that from the
// line's first character, and the scan for it used to know only space and
// tab -- so a file saved with a UTF-8 byte order mark lost its first line
// without one word of complaint.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // Each case writes its own file, so two of them cannot read each other's
    // leftovers, and the stem is what the module guard registers -- a name
    // shared with a standard-library module would make the second import of
    // that module a silent no-op.
    fs::path write_script(const std::string& name, const std::string& body)
    {
        const fs::path path = fs::temp_directory_path() / ("zelph_session_" + name);
        std::ofstream(path, std::ios::binary) << body;
        return path;
    }
}

TEST_CASE("session script: a file named on the command line echoes its statements")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const fs::path script = write_script("echo.zph", "a rel b\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        // The echo is the observable half; the half that matters is that it
        // is also what records the last node (see the next case).
        CHECK(any_output_contains(collector, "a rel b")); });
}

TEST_CASE("session script: a bare .node names the node of the statement before it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The exact shape that failed: `zelph s7_figure1.zph` reported "No
        // argument given and no previous output node available" while
        // `zelph < s7_figure1.zph` drew the figure, and the difference was
        // the suppressed echo. The paper harness compensated by refusing to
        // use the file form at all.
        const fs::path script = write_script("lastnode.zph", "x rel y\n.node\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "Representation: x rel y"));
        CHECK_FALSE(any_event_contains(collector, "no previous output node")); });
}

TEST_CASE("session script: an .import inside it still hides the module's own lines")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The session is only the file's OWN lines. A module it imports is
        // still a library load -- otherwise importing the standard library
        // from a script would print the whole standard library.
        const fs::path module = write_script("quiet_module.zph", "hidden rel thing\n");
        const fs::path script = write_script("importer.zph",
                                             "visible rel thing\n.import " + module.string() + "\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);
        fs::remove(module);

        CHECK(any_output_contains(collector, "visible rel thing"));
        CHECK_FALSE(any_output_contains(collector, "hidden rel thing")); });
}

TEST_CASE("session script: its statements anchor the deduction focus")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // With input capture suppressed the default filter had no anchor at
        // all and dropped every deduction, so the file form wrote nothing to
        // stdout where the piped form printed the derivation. Both forms are
        // compared here against each other rather than against a literal, so
        // the case says what it is actually about: the two are the same run.
        interactive.process(".deductions focus");
        const fs::path script = write_script("focus.zph",
                                             "u p1 v\n(A p1 B) => (A p2 B)\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_deduction_of(collector, "u p2 v")); });
}

TEST_CASE("session script: auto-run fires per line, so a later line sees the consequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A module runs once when it has been read in full, which is right
        // for a library. In a session the run belongs after each line, and
        // the third line below can only see the consequence if the second
        // one has already fired. Asked through zelph/exists rather than
        // through a query, because asking IS asserting for a query and the
        // question would then answer itself.
        const fs::path script = write_script(
            "autorun.zph",
            "m p1 n\n"
            "(A p1 B) => (A p2 B)\n"
            R"js(%(zelph/out (string "DERIVED-" (zelph/exists "m" "p2" "n"))))js"
            "\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "DERIVED-true")); });
}

TEST_CASE("session script: .quit ends the file where it stands")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // `.quit` was a silent no-op in a file: the command table registers
        // it as an empty handler because the REPL loop intercepts the line
        // itself, and no other loop was looking. A script that ended with
        // `.quit` -- which is how a REPL transcript ends -- therefore kept
        // running past it.
        const fs::path script = write_script("quit.zph", "before rel it\n.quit\nafter rel it\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "before rel it"));
        CHECK_FALSE(any_event_contains(collector, "after rel it")); });
}

TEST_CASE("session script: an unknown extension is run, an imported one is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A `#!/usr/bin/env zelph` script is conventionally called `report`
        // or `check-facts`, not `check-facts.zph`, and every other
        // interpreter runs the file it is pointed at whatever it is named.
        // `.import` keeps the rule, because there the name IS the interface:
        // it is resolved against the standard library, and a ".json" behind
        // it is a mistake worth refusing before the file is fed to the parser
        // line by line.
        const fs::path odd = write_script("named.tool", "tool rel ran\n");
        collector.clear();
        interactive.process_file(odd.string());
        CHECK(any_output_contains(collector, "tool rel ran"));

        collector.clear();
        CHECK_THROWS(interactive.process(".import " + odd.string()));
        fs::remove(odd); });
}

TEST_CASE("session script: a failed import is recorded as a failure of the session")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // The exit status the binary reports is this flag. It used to be
        // absent altogether: whatever happened, zelph left with 0, so a
        // script whose `.import` did not resolve reported success -- and
        // because the statements after it still answered (they never needed
        // the missing module), the log looked entirely plausible while
        // describing a configuration nobody had asked for.
        CHECK_FALSE(interactive.had_failure());

        const fs::path script = write_script("badimport.zph", ".import no-such-module-here\n");
        CHECK_THROWS(interactive.process_file(script.string()));
        fs::remove(script);

        // The library records only what it can see itself; the throw above is
        // what the caller reports and turns into a failure. Both halves are
        // needed, so both are pinned.
        interactive.note_failure();
        CHECK(interactive.had_failure());

        // .new resets the graph, not what the session has already got wrong.
        interactive.process(".new");
        CHECK(interactive.had_failure()); });
}

TEST_CASE("session script: a declined import is a failure without an exception")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // The one path that fails the user's request and throws NOTHING: two
        // scripts claim the same module ID, the second is declined, and the
        // session that follows is not the one that was asked for. An exit
        // status derived from exceptions alone would report this as success.
        const fs::path first  = write_script("provider_a.zph", ".provides sharedid\na rel one\n");
        const fs::path second = write_script("provider_b.zph", ".provides sharedid\nb rel two\n");

        interactive.process(".import " + first.string());
        CHECK_FALSE(interactive.had_failure());

        interactive.process(".import " + second.string());
        CHECK(interactive.had_failure());

        fs::remove(first);
        fs::remove(second); });
}

// --- What a line IS, when something invisible stands in front of it --------

TEST_CASE("invisible prefixes: a byte order mark is part of neither the first line nor its first name")
{
    // A .zph file saved with a BOM used to lose its first line in silence.
    // The three bytes made the line miss both the '#' test and the '.' test,
    // so `.import x` was read as the START of a statement, stayed
    // incomplete, and the NEXT line was appended to it: two lines of the
    // script became one fact, with a node named "\xEF\xBB\xBF.import" in the
    // graph and not one error message anywhere.
    //
    // After the line test skipped the mark, a statement on the first line
    // still arrived at the parser with the mark positioned ahead, causing
    // the subject of the first fact to be a node named "\xEF\xBB\xBFp". Its
    // echo reads `p rel q`, which is all this case used to check, whereas a
    // query for `p` yielded no results. A module is read by the same loop
    // as a script, and it produces no echo, meaning that there a query is
    // the sole method to see the fact.
    const auto check_first_fact = [](auto& collector, auto& interactive)
    {
        collector.clear();
        interactive.process("p rel A");
        CHECK(answers_contain(collector, "p rel q"));

        // The complete answer set, so that a subject written using the
        // mark at the beginning cannot be accepted as `p`.
        collector.clear();
        interactive.process("A rel q");
        CHECK(collect_answers(collector) == std::vector<std::string>{"p rel q"});
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("a command in the first line")
        {
            const fs::path module = write_script("bom_imported.zph", "m q n\n");
            const fs::path script = write_script("bom_command.zph",
                                                 "\xEF\xBB\xBF" ".import " + module.string() + "\nr rel s\n");
            interactive.process_file(script.string());
            fs::remove(script);
            fs::remove(module);

            collector.clear();
            interactive.process("m q A");
            CHECK(answers_contain(collector, "m q n"));

            // The tell that the two lines had been glued together.
            collector.clear();
            interactive.process("r rel A");
            CHECK(answers_contain(collector, "r rel s"));
        }
        SUBCASE("a statement in the first line of a script named on the command line")
        {
            const fs::path script = write_script("bom.zph", "\xEF\xBB\xBF" "p rel q\n");
            interactive.process_file(script.string());
            fs::remove(script);
            check_first_fact(collector, interactive);
        }
        SUBCASE("a statement in the first line of a module")
        {
            const fs::path module = write_script("bom_module.zph", "\xEF\xBB\xBF" "p rel q\n");
            interactive.process(".import " + module.string());
            fs::remove(module);
            check_first_fact(collector, interactive);
        }
        SUBCASE("a result query in the first line")
        {
            // The '?' prefix is searched for at the beginning of the
            // statement, now situated behind the mark. As the mark stayed
            // in front, the '?' became the second character within a name:
            // the line asserted two facts regarding a node named
            // "\xEF\xBB\xBF?", with `x` serving as the relation and `p` and
            // `y` as the objects, while `x p y` never made it into the
            // graph. The subcases mentioned above lack a '?', so a prefix
            // test applied directly to the raw line would succeed for all
            // of them.
            const fs::path script = write_script("bom_query.zph", "\xEF\xBB\xBF" "? x p y\n");
            interactive.process_file(script.string());
            fs::remove(script);
            CHECK_FALSE(interactive.is_accumulating());

            collector.clear();
            interactive.process("x p A");
            CHECK(collect_answers(collector) == std::vector<std::string>{"x p y"});
        } });
}

TEST_CASE("invisible prefixes: a no-break space in front of a command still runs the command")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Same family, and the one a copy out of a PDF or an editor's
        // auto-indent produces. U+00A0 is whitespace to every reader and was
        // whitespace to nothing here.
        collector.clear();
        interactive.process("\xC2\xA0.lang wikidata");
        CHECK_FALSE(any_event_contains(collector, "Could not parse statement"));
        CHECK(interactive.get_lang() == "wikidata");
        interactive.process(".lang zelph"); });
}

TEST_CASE("invisible prefixes: no other invisible space in front of a statement is part of it")
{
    // A statement is read from the position where the line scan stopped,
    // and the scan bypasses each character that whitespace_length knows,
    // not merely U+FEFF. The byte order mark scenarios mentioned earlier
    // pin the mark by itself; these two characters represent the rest, and
    // previously each went wrong in its distinct manner.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a no-break space")
        {
            // A bare name cannot include U+00A0: its first byte matches
            // the first byte of '¬', and the grammar reserves bytes, not
            // characters. Thus, this line triggered a syntax error,
            // halting the script, even though a command located behind
            // that same character had already executed (refer to the case
            // above).
            CHECK_NOTHROW(interactive.process("\xC2\xA0p rel q"));
        }
        SUBCASE("an em space")
        {
            // As with the byte order mark, U+2003 was incorporated into
            // the first name, making the fact pertain to a node named
            // "\xE2\x80\x83p".
            CHECK_NOTHROW(interactive.process("\xE2\x80\x83p rel q"));
        }

        // The complete answer set, so that a subject retaining the space
        // ahead cannot be considered as `p`.
        collector.clear();
        interactive.process("A rel q");
        CHECK(collect_answers(collector) == std::vector<std::string>{"p rel q"}); });
}

TEST_CASE("invisible prefixes: an indented comment is a comment")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The comment test used to demand column 0, so an indented '#' line
        // -- which is how a comment inside a block is written -- was parsed
        // as a fact whose subject is named "#". Nothing complained; the graph
        // simply grew a statement out of a remark.
        collector.clear();
        interactive.process("    # this is a remark, not a fact");
        CHECK_FALSE(any_output_contains(collector, "remark"));
        CHECK_FALSE(any_event_contains(collector, "Could not parse statement")); });
}

// ---------------------------------------------------------------------------
// A comment can also end a line. When not within a quoted name, a '#'
// character positioned at the start of a line or immediately following a
// space or tab starts a comment that runs to the end of that line; a '#'
// embedded within a token -- such as C#, a#b, or part of an IRI -- is treated
// as regular text. Previously, only entire lines could serve as comments: the
// line `x ~ symvar # variables` asserted x ~ #, x ~ variables, and x ~
// symvar, a comma within a comment transformed the line into a conjunction,
// and `.save file.bin # keep it` was rejected due to an extra argument.
// ---------------------------------------------------------------------------

TEST_CASE("comments: a # after whitespace starts a comment at the end of a statement")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("after spaces, and after a tab")
        {
            interactive.process("x ~ symvar   # variables, not a set");
            interactive.process("y ~ symconst\t# constants");
            collector.clear();
            interactive.process("x ~ A");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "x ~ symvar"));
            collector.clear();
            interactive.process("y ~ A");
            CHECK(collect_answers(collector).size() == 1);
        }
        SUBCASE("in every line of a statement written over several lines")
        {
            interactive.process("(X q Y, # the first condition");
            interactive.process(" Y q Z)   # the second");
            interactive.process("=> (X q2 Z) # the consequence");
            interactive.process("a q b");
            interactive.process("b q c");
            collector.clear();
            interactive.process("A q2 B");
            CHECK(answers_contain(collector, "a q2 c"));
        }
        SUBCASE("a statement cut short by a comment waits for the rest")
        {
            interactive.process("a p   # the object follows");
            CHECK(interactive.is_accumulating());
            interactive.process("b");
            collector.clear();
            interactive.process("a p X");
            CHECK(answers_contain(collector, "a p b"));
            // The number is what pins the comment rule. While only the
            // completeness test skipped comments, the statement remained
            // pending, yet the parser continued to interpret the
            // comment's words as objects: five answers rather than a
            // single one.
            CHECK(collect_answers(collector).size() == 1);
        }
        SUBCASE("at the end of the input, a comment does not complete a statement")
        {
            // finish_input eliminates the comments before making its
            // decision, as the completeness test no longer skips them.
            // Without this step, each `#` and `c` would be treated as an
            // individual object, causing the last line of a script to assert
            // two facts that nobody authored, rather than being reported as
            // unfinished.
            interactive.process("a rel   # c");
            CHECK_THROWS_WITH_AS(interactive.finish_input(),
                                 doctest::Contains("unfinished statement"),
                                 std::runtime_error);
            collector.clear();
            interactive.process("a rel X");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("brackets and quotes inside a comment do not hold the statement open")
        {
            interactive.process("a p b # (");
            CHECK_FALSE(interactive.is_accumulating());
            interactive.process("c p d # \"");
            CHECK_FALSE(interactive.is_accumulating());
            collector.clear();
            interactive.process("S p O");
            CHECK(collect_answers(collector).size() == 2);
        }
        SUBCASE("the self-fact prefix")
        {
            interactive.process(":m x # a marker");
            collector.clear();
            interactive.process("x m A");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "x m x"));
        } });
}

TEST_CASE("comments: the ? prefix takes a trailing comment")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    collector.clear();
    interactive.process("? &2 + &3   # the sum");
    CHECK(answers_contain(collector, "(&2 + &3) = &5"));
}

TEST_CASE("comments: a # inside a name or a quoted name is text")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const char* const lines[] = {
            "C# ~ language",
            "a#b p c",
            "(x#1 p y) q r",
            "x p (y#z q w)",
            "http://x.org/a#b q c",
            "(\"#tag\" p x) q r",
            // A '#' immediately following '(' resides within a token, making
            // this name "#hashtag" and causing the statement to close. If
            // read as a comment, the '#' would leave the bracket open.
            "(#hashtag p x) q t",
            "x p \"a # b\"",
            // The escaped quotes on this line form a matching pair before
            // the '#', ensuring the line is interpreted properly even when
            // a backslash does not escape the next character. It pins the
            // display of escaped quotes, not the act of escaping itself.
            "x p \"say \\\"hi\\\" # x\"",
            // This one needs the escape: otherwise the quoted name ends at
            // the escaped quote, and the '#' behind it starts a comment.
            "x p \"a \\\" # b\""};
        for (const char* const line : lines)
        {
            CAPTURE(line);
            interactive.process(line);
            CHECK_FALSE(interactive.is_accumulating());
        }

        collector.clear();
        interactive.process("C# ~ X");
        CHECK(answers_contain(collector, "C# ~ language"));
        collector.clear();
        interactive.process("S q c");
        CHECK(collect_answers(collector).size() == 1);
        // The entire name remains intact, and it is the name provided by
        // the quoted spelling: both quoted and unquoted forms,
        // "#hashtag" constitutes a single node.
        collector.clear();
        interactive.process("S q t");
        CHECK(answers_contain(collector, "(\"#hashtag\" p x) q t"));
        collector.clear();
        interactive.process("x p O");
        CHECK(any_output_contains(collector, "a # b"));
        CHECK(any_output_contains(collector, "# x"));
        CHECK(any_output_contains(collector, "a \\\" # b"));

        // A name enclosed in quotation marks can extend across multiple
        // lines; the '#' appearing on its second line is considered part of
        // the text.
        interactive.process("m n \"line one");
        interactive.process("line # two\"");
        CHECK_FALSE(interactive.is_accumulating());
        collector.clear();
        interactive.process("m n O");
        CHECK(any_output_contains(collector, "line # two")); });
}

TEST_CASE("comments: a continuation line is still classified by its first character")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Even within an open quoted name, any line beginning with a '#'
        // constitutes a comment line, while one starting with a '.'
        // represents a command: the nature of a line is determined before
        // examining the statement it might extend. Thus, the '#' line below
        // is discarded entirely, including the closing quote, and the
        // statement remains pending. The rule remains a single
        // straightforward rule, and a quoted name that extends across
        // multiple lines offers no use worth making an exception for, since
        // the line break is not included in the name.
        interactive.process("m n \"line one");
        interactive.process("# two\"");
        CHECK(interactive.is_accumulating());

        // The command runs, with the note that a statement remains
        // pending.
        collector.clear();
        interactive.process(".lang wikidata");
        CHECK(interactive.get_lang() == "wikidata");
        CHECK(any_event_contains(collector, "still inside an unfinished statement"));
        interactive.process(".lang zelph");
        CHECK(interactive.is_accumulating()); });
}

TEST_CASE("comments: a command takes a trailing comment")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".lang wikidata   # switch the language");
        CHECK(interactive.get_lang() == "wikidata");
        interactive.process(".lang zelph # and back");
        CHECK(interactive.get_lang() == "zelph");

        interactive.process(".deductions all   # print everything");
        collector.clear();
        interactive.process("a p b");
        interactive.process("c p d");
        interactive.process("(X p Y) => (Y p2 X)");
        CHECK(any_deduction_of(collector, "b p2 a"));

        CHECK_NOTHROW(interactive.process(".cleanup   # tidy up"));

        // It really prunes: the comment does not become part of the pattern.
        interactive.process(".prune-facts A p B   # remove them");
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        // A name starting with '#' must be enclosed in quotes, as at the
        // start of a line; without quotation, the argument functions as a
        // comment, and the command is short.
        interactive.process("e f g");
        CHECK_NOTHROW(interactive.process(".name g \"#hash\""));
        CHECK_THROWS(interactive.process(".name g #hash")); });
}

TEST_CASE("comments: Janet keeps Janet's own rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("%(zelph/out \"J-INLINE\")   # a Janet comment");
        CHECK(any_output_contains(collector, "J-INLINE"));

        // A bare '%' followed by a comment still toggles block mode.
        interactive.process("%   # into Janet");
        interactive.process("(zelph/out \"J-BLOCK\")");
        collector.clear();
        // A backtick string stands as the sole input where the two
        // comment rules diverge: in Janet's interpretation, the '#'
        // within it constitutes text, whereas zelph's rule would cut the
        // line at that point and leave the string open. Every other line
        // in this context is processed identically under both rules.
        interactive.process("(zelph/out `K # L`)");
        interactive.process("%   # back to zelph");
        CHECK(any_output_contains(collector, "J-BLOCK"));
        CHECK(any_output_contains(collector, "K # L"));
        interactive.process("s t u");
        collector.clear();
        interactive.process("s t X");
        CHECK(answers_contain(collector, "s t u")); });
}

TEST_CASE("comments: Janet's own reader decides whether an inline Janet line is complete")
{
    // The completeness of a `%(...)` line was decided by a scan that
    // recognized double-quoted strings and '#' comments but overlooked
    // Janet's backtick strings. A '#' or a '(' within one left the
    // expression open, causing every subsequent line to be collected as
    // Janet, so the zelph statement that came after never ran. Each `%` line
    // below is evaluated independently by that test, and is_accumulating()
    // reflects its outcome.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a '#' inside a backtick string is text")
        {
            collector.clear();
            interactive.process("%(zelph/out `J # B`)");
            CHECK_FALSE(interactive.is_accumulating());
            CHECK(any_output_contains(collector, "J # B"));
            interactive.process("v w x");
            collector.clear();
            interactive.process("v w X");
            CHECK(answers_contain(collector, "v w x"));
        }
        SUBCASE("so is a '#' inside a string of two backticks, with a single one inside it")
        {
            collector.clear();
            interactive.process("%(zelph/out ``J ` # B``)");
            CHECK_FALSE(interactive.is_accumulating());
            CHECK(any_output_contains(collector, "J ` # B"));
        }
        SUBCASE("a bracket or a double quote inside a backtick string is not counted")
        {
            interactive.process("%(zelph/out `J ( B`)");
            CHECK_FALSE(interactive.is_accumulating());
            interactive.process("%(zelph/out `say \"hi`)");
            CHECK_FALSE(interactive.is_accumulating());
        }
        SUBCASE("a backtick string that spans lines keeps the expression open")
        {
            // The ')' within the string used to close the expression on the
            // first line, thus Janet received a string lacking its closing
            // part.
            collector.clear();
            interactive.process("%(zelph/out `a)");
            CHECK(interactive.is_accumulating());
            interactive.process("b`)");
            CHECK_FALSE(interactive.is_accumulating());
            CHECK(any_output_contains(collector, "a)"));
        }
        SUBCASE("an open backtick string is incomplete, an extra ')' is not")
        {
            // A syntax error counts as complete, so that Janet issues a
            // report when the code runs, rather than the REPL remaining
            // idle for additional input.
            interactive.process("%(zelph/out `J");
            CHECK(interactive.is_accumulating());
            interactive.process("`)");
            CHECK_FALSE(interactive.is_accumulating());

            CHECK_THROWS(interactive.process("%(zelph/out \"x\"))"));
            CHECK_FALSE(interactive.is_accumulating());
        }
        SUBCASE("a trailing comment and a bare symbol still complete the line")
        {
            // The reader receives the line, followed by a newline. Absent
            // that newline, it would remain within the comment or within the
            // symbol, awaiting further input.
            interactive.process("%(def inline-v 7)   # a Janet comment");
            CHECK_FALSE(interactive.is_accumulating());
            collector.clear();
            interactive.process("%inline-v");
            CHECK_FALSE(interactive.is_accumulating());
            CHECK(any_output_contains(collector, "7"));
        } });
}

TEST_CASE("Janet over several lines: what was collected is let go before its code runs")
{
    // An open `%(` expression maintains state: the lines gathered up to this
    // point, and the reader that has read them. There exists a single reader
    // across all lines, since passing the entire buffer to a new reader for
    // each line made time and memory usage increase quadratically with the
    // expression's length. Both elements are released before code execution,
    // just as occurs when a `%` block is closed, so that failing code cannot
    // leave them behind.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("code that throws leaves no expression open")
        {
            // The state was previously left solely after the code executed,
            // meaning that code which threw kept the REPL collecting: the
            // next line was appended to it, and the entirety was executed
            // once more, including a zelph statement.
            interactive.process("%(zelph/out");
            CHECK(interactive.is_accumulating());
            CHECK_THROWS(interactive.process("  (error \"inline-boom\"))"));
            CHECK_FALSE(interactive.is_accumulating());
            interactive.process("x y z");
            collector.clear();
            interactive.process("x y Z");
            CHECK(answers_contain(collector, "x y z"));
        }
        SUBCASE("a module that ends inside one leaves no expression open")
        {
            // The end of a module runs what the module had left open, which
            // constitutes a syntax error in this instance. The state used to
            // persist beyond that error, causing the session's subsequent
            // lines to be added to the module's expression.
            const fs::path module = write_script("open_inline.zph", "%(zelph/out \"never closed\"\n");
            CHECK_THROWS(interactive.process(".import " + module.string()));
            fs::remove(module);
            CHECK_FALSE(interactive.is_accumulating());

            collector.clear();
            interactive.process("%(zelph/out \"fresh\")");
            CHECK_FALSE(interactive.is_accumulating());
            CHECK(any_output_contains(collector, "fresh"));
        }
        SUBCASE("a module that ends inside a % block leaves no block open")
        {
            // The same end of a module runs a `%` block that remained
            // unclosed. Upon failure of that code, the session stayed in
            // block mode, and its own statements were collected as
            // Janet.
            const fs::path module = write_script("open_block.zph", "%\n(zelph/out \"never closed\"\n");
            CHECK_THROWS(interactive.process(".import " + module.string()));
            fs::remove(module);
            CHECK_FALSE(interactive.is_accumulating());

            interactive.process("x y z");
            collector.clear();
            interactive.process("x y Z");
            CHECK(answers_contain(collector, "x y z"));
        }
        SUBCASE("a module that the code imports reads its own lines")
        {
            // While the code was executing, the expression remained
            // classified as open, causing each line within a .zph module
            // loaded via `zelph/import` to be collected as Janet: the
            // module's statements were compiled as Janet code, the outer
            // code ran a second time, and the import encountered failure.
            // The path is inserted into a Janet string, thus it is
            // expressed using forward slashes.
            const fs::path module = write_script("inline_import.zph", "mm p vv\n");
            interactive.process("%(do");
            CHECK(interactive.is_accumulating());
            CHECK_NOTHROW(interactive.process("  (zelph/import \"" + module.generic_string() + "\"))"));
            fs::remove(module);
            CHECK_FALSE(interactive.is_accumulating());

            collector.clear();
            interactive.process("mm p A");
            CHECK(answers_contain(collector, "mm p vv"));
        } });
}

TEST_CASE("comments: the command tokenizer stops at a comment")
{
    // The identical tokenizer performs a pre-scan on a module's .provides
    // lines straight from the file, before the module runs: "note" must not
    // be interpreted as a module ID.
    CHECK(zelph::string::tokenize_quoted(".provides a   # note") == std::vector<std::string>{".provides", "a"});
    CHECK(zelph::string::tokenize_quoted(".name b \"#foo\"") == std::vector<std::string>{".name", "b", "#foo"});
    CHECK(zelph::string::tokenize_quoted(".load hf://x/y.json#frag") == std::vector<std::string>{".load", "hf://x/y.json#frag"});
    CHECK(zelph::string::tokenize_quoted("# all of it").empty());
}
