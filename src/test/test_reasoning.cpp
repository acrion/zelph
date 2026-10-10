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

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace zelph::test;

TEST_CASE("import: missing scripts fail with a standard-library hint, wrong extensions are rejected")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process(".import definitely-not-a-zelph-script"),
                             doctest::Contains("standard library"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".import foo.txt"), std::runtime_error); });
}

// ---------------------------------------------------------------------------
// Predicate parsing
// ---------------------------------------------------------------------------

TEST_CASE("parsing: dot-dot predicate")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "g .. h\nh .. i");
        // A name beginning with '.' prints enclosed in quotes:
        // positioned at the start of a line, it would be read as a
        // command. See needs_quotes.
        CHECK(any_output_starts_with(collector, "g \"..\" h"));
        CHECK(any_output_starts_with(collector, "h \"..\" i")); });
}

TEST_CASE("parsing: arrow predicates")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
atom_A => atom_B
atom_C <= atom_D
)");
        CHECK(any_output_starts_with(collector, "atom_A => atom_B"));
        CHECK(any_output_starts_with(collector, "atom_C <= atom_D")); });
}

TEST_CASE("parsing: a bare name may contain any character but the reserved ones")
{
    // The grammar's reserved set is a Janet PEG `set` that matches single
    // BYTES, and `¬` corresponds to the two-byte sequence C2 AC. Listed in
    // the set, it reserved both: each character from U+0080 to U+00BF
    // begins with C2, and numerous others carry AC as a continuation byte
    // -- for instance, `€` is encoded as E2 82 AC, and the 京 in 北京 appears
    // as E4 BA AC. Consequently, a bare name ended at `°C`, `µ`, `½`, `€`,
    // or 北京, causing the line to fail parsing. The grammar now reserves the
    // CHARACTER `¬`, and these are treated as ordinary names in every
    // position.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_NOTHROW(interactive.process("°C µ €"));
        CHECK_NOTHROW(interactive.process("€ ½ °C"));
        CHECK_NOTHROW(interactive.process("½ µ 北京"));
        CHECK_NOTHROW(interactive.process("北京 µg/m³ ½"));

        collector.clear();
        interactive.process("S µ O");
        std::vector<std::string> printed = collect_answers(collector);
        std::sort(printed.begin(), printed.end());
        CHECK(printed == std::vector<std::string>{"°C µ €", "½ µ 北京"});

        collector.clear();
        interactive.process("北京 P O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"北京 µg/m³ ½"});

        collector.clear();
        interactive.process("S P °C");
        CHECK(collect_answers(collector) == std::vector<std::string>{"€ ½ °C"});

        // The `¬` symbol is still reserved, precisely as the character it is:
        // when positioned at the start of a value, it turns the condition
        // into a negated form, and similarly when placed after a name made of
        // the aforementioned characters. The negated condition holds only for
        // ½ and 北京, since `€ ½ °C` is there.
        interactive.process("(X µ Y, ¬(Y ½ X)) => (X ok Y)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("X ok Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"½ ok 北京"});

        // ... and it continues to terminate a bare name at any location it
        // occupies, furthermore immediately after a character that starts
        // with the same byte C2.
        CHECK_THROWS_AS(interactive.process("x¬y rel z"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process("°C¬ rel z"), std::runtime_error);

        // The no-break space U+00A0 also starts with C2. It stays
        // reserved: a line typed or copied with it instead of a blank
        // is refused, not read as a single name containing the blanks.
        // Likewise, a name positioned before it is not reported as glued
        // to the parenthesis located behind it -- a blank in that spot
        // would not make the line parse.
        CHECK_THROWS_AS(interactive.process("x\xC2\xA0y rel z"), std::runtime_error);
        std::string message;
        try
        {
            interactive.process("x\xC2\xA0(a b c) rel z");
        }
        catch (const std::runtime_error& e)
        {
            message = e.what();
        }
        CHECK(message.find("Could not parse") != std::string::npos);
        CHECK(message.find("glued") == std::string::npos); });
}

TEST_CASE("parsing: a statement continues on the next line behind a name such as °C")
{
    // A statement is considered complete when it contains three tokens at
    // the top level, or when it consists of a single token that stands as a
    // value in itself: a set, a list, a focus, or a negation `¬(...)`. The
    // negation was identified by its first BYTE, C2, which is shared by
    // every character in the range U+0080 to U+00BF, meaning that a line
    // holding only the name °C was treated as a fully formed statement, and
    // the subsequent line initiated a new one. A bare name remains pending
    // until the rest of its statement is provided, regardless of what its
    // first character may be.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("°C");
        interactive.process("µ €");
        collector.clear();
        interactive.process("S µ O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"°C µ €"});

        // The identical case applies to the second token located after
        // the result-query prefix.
        interactive.process("? °C");
        interactive.process("µ €");
        collector.clear();
        interactive.process("S µ O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"°C µ €"});

        // A lone negation remains a statement in its own right: the line
        // following it constitutes a fact, not the rest of the negation.
        interactive.process("¬(°C q €)");
        interactive.process("½ q µ");
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"½ q µ"}); });
}

TEST_CASE("rules: asking which implications exist does not create a rule")
{
    // `=>` is an ordinary relation type as well as the rule arrow (see the
    // case above), so asking about implications is an ordinary query -- and
    // in a system whose point is reasoning ABOUT statements it is a question
    // one asks. Entering it materializes the pattern `S => O`, which
    // get_rules then counted as a rule of the network: .stat said "Rules: 1"
    // and .list-rules showed the query, permanently, on a graph nobody had
    // written a rule for. A condition that is only a variable binds nothing,
    // so it could not fire either.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");

        collector.clear();
        interactive.process("S => O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 0"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        // ... and the query still answers what it is asked. An arrow fact
        // between two atoms is a fact AND passes as a rule (its condition is
        // not a variable), which is why the count is asked before it.
        interactive.process("atom_A => atom_B");
        collector.clear();
        interactive.process("S => O");
        CHECK(answers_contain(collector, "atom_A => atom_B"));

        // A real rule is unaffected, and still fires.
        interactive.process("m p n");
        interactive.process("(X p Y) => (X q Y)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("M q N");
        CHECK(answers_contain(collector, "m q n"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p Y) => (X q Y)"));
        CHECK_FALSE(any_output_contains(collector, "S => O"));
        CHECK_FALSE(any_output_contains(collector, "atom_A => atom_B"));

        // ... which is what keeps .remove-rules from deleting DATA. An arrow
        // fact between two atoms cannot fire -- an atom is not something that
        // holds -- and a command that says it removes rules must not take it.
        interactive.process(".remove-rules");
        collector.clear();
        interactive.process("S => O");
        CHECK(answers_contain(collector, "atom_A => atom_B"));

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 0")); });
}

TEST_CASE("rules: .remove-rules also removes the rules that only removed rules mentioned")
{
    // The rule contained within a rule generator is a mention, not a rule in
    // force: it is an object of the generator (Zelph::is_mentioned). After
    // the generator ceases to exist, nothing mentions it anymore, and a `=>`
    // fact that is a part of nothing is a rule. Removing solely the rules
    // listed when the command starts would bring it into force: .list-rules
    // would show it, and it would fire, following a command that says it
    // removes every rule.
    const auto nothing_left = [](auto& collector, auto& interactive)
    {
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        collector.clear();
        process_lines(interactive, "now go k\nb p k");
        CHECK_FALSE(any_deduction_of(collector, "likes"));

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty());
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("a generator that has not fired")
        {
            interactive.process("(G go H) => ((X p H) => (X likes H))");
            interactive.process(".remove-rules");
            nothing_left(collector, interactive);
        }
        SUBCASE("a generator that has fired")
        {
            // The firing's instance constitutes a rule in force and goes
            // with the generator; the rule the generator mentions goes
            // after both.
            process_lines(interactive, "(G go H) => ((X p H) => (X likes H))\nnow go k");
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(any_output_contains(collector, "(X p k) => (X likes k)"));

            interactive.process(".remove-rules");
            nothing_left(collector, interactive);
        }
        SUBCASE("a generator of generators")
        {
            // Each rule removal frees the rule one level deeper, so the
            // rules are read again until no rule remains, not merely one
            // additional time.
            interactive.process("(A on B) => ((G go H) => ((X p H) => (X likes H)))");
            interactive.process(".remove-rules");
            nothing_left(collector, interactive);
        } });
}

TEST_CASE("rules: .remove-rules leaves a rule that a statement mentions")
{
    // Reading the rules once more following a removal must stop at the
    // rules that only removed rules mentioned. A `=>` fact that a
    // statement about it mentions is not in force, and that statement is
    // not a rule, so it continues to mention the `=>` fact after the
    // removal: the fact is neither included in the list nor removed, and
    // it does not fire. The generator beside it makes the command read the
    // rules a second time.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((X p H) => (X likes H)) is noted
(G go H) => ((X q H) => (X hates H))
)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(any_output_contains(collector, "(G go H) => ((X q H) => (X hates H))"));
        CHECK_FALSE(any_output_contains(collector, "(X likes H)"));

        interactive.process(".remove-rules");

        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "(X likes H)"));

        collector.clear();
        interactive.process(".explain ((X p H) => (X likes H))");
        CHECK(any_output_contains(collector, "[rule mentioned; not in force]"));

        collector.clear();
        interactive.process("b p k");
        CHECK_FALSE(any_deduction_of(collector, "likes"));

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).empty()); });
}

// NOTE: there is no biconditional arrow in the grammar. `<=>` is read as the
// list <=>, which then sits in predicate position; that it renders as such is
// pinned by "display: a list in predicate position renders as a list" in
// test_node_display.cpp. It used to print as "??".

// ---------------------------------------------------------------------------
// Sequences and lists
// ---------------------------------------------------------------------------

TEST_CASE("parsing: compact sequence")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "seq_compact is_defined_as <123>");
        // Compact input builds the list LSB-first; the display no longer
        // reverses it -- that convention belongs to registered numerals.
        CHECK(any_output_contains(collector, "<3 2 1>")); });
}

TEST_CASE("parsing: spaced sequence")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "seq_spaced is_defined_as < seqItem1 seqItem2 seqItem3 >");
        CHECK(any_output_contains(collector, "<seqItem1 seqItem2 seqItem3>")); });
}

TEST_CASE("parsing: quoted sequence keeps its order")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(quoted_sequence ~ < "a" "b" "c" >)");
        CHECK(any_output_contains(collector, "<a b c>")); });
}

// ---------------------------------------------------------------------------
// Nested structures
// ---------------------------------------------------------------------------

TEST_CASE("parsing: an empty container denotes nil")
{
    // `<>` is the empty cons list, and the empty cons list IS the
    // terminator every list ends at; Zelph::set() has always answered nil
    // for the empty set. The parser used to answer neither: it produced
    // Janet's nil, which zelph/fact reads as "no object", so the whole
    // statement vanished without a word.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the empty node list")
        {
            process_lines(interactive, "q p <>");
            CHECK(any_output_contains(collector, "q p nil"));
        }
        SUBCASE("the empty set")
        {
            process_lines(interactive, "q p {}");
            CHECK(any_output_contains(collector, "q p nil"));
        }
        SUBCASE("all three spellings are the same node")
        {
            process_lines(interactive, "q p <>\nq p {}\nq p nil");
            collector.clear();
            interactive.process("q p X");
            std::size_t answers = 0;
            for (const auto& e : collector.events())
                if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
            CHECK(answers == 1);
        } });
}

TEST_CASE("parsing: nested sequence in set")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "nested_seq_in_set holds { <setElem1 setElem2> <setElem3 setElem4> }");
        CHECK(any_output_contains(collector, "<setElem1 setElem2>"));
        CHECK(any_output_contains(collector, "<setElem3 setElem4>")); });
}

TEST_CASE("parsing: mixed container")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(mixed_container content < (myCond => myDeduct) (myDeduct2 <= myCond2) { setElem5 setElem6 } "literal string" >)");
        CHECK(any_output_contains(collector, "myCond => myDeduct"));
        CHECK(any_output_contains(collector, "myDeduct2 <= myCond2"));
        CHECK(any_output_contains(collector, "setElem5"));
        CHECK(any_output_contains(collector, "setElem6")); });
}

TEST_CASE("parsing: deep nesting")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(deep_nesting ~ ( Level1 ( Level2 ( Level3 predicate "Level3Object" ) Level2Object) Level1Object))");
        CHECK(any_output_contains(collector, "Level1"));
        CHECK(any_output_contains(collector, "Level1Object")); });
}

TEST_CASE("parsing: a term nested 20 deep is read once, not twice per enclosing group")
{
    // A parenthesized group consists of either a single statement or a comma
    // list of conditions, with the grammar determining the correct form by
    // testing both options in sequence: first the comma list, and if no comma
    // was detected, then the whole group as a statement. Each attempt read
    // every internal group identically, meaning a term nested n levels deep
    // was read 2^n times -- and since Janet does not discard intermediate
    // results during a match, each of those evaluations remained in memory
    // until the line completed. At a nesting depth of 20, this single line
    // took six seconds and one gigabyte in the REPL; a term nested 30 levels
    // deep had already used 68 GB before being terminated. No external data
    // needed to be loaded: the parser alone was responsible for this
    // behaviour.
    //
    // The parser does not count its work, making this a time bound,
    // positioned far from either side: this test recorded 8.6 s before the
    // fix and no more than 5 ms afterwards, both measurements pinned to an
    // efficiency core.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    std::string term = "x";
    for (int depth = 0; depth < 20; ++depth)
        term = "(exp of " + term + ")";

    const auto start = std::chrono::steady_clock::now();
    interactive.process(term + " p q");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    CHECK(any_output_contains(collector, term + " p q"));
    CHECK(elapsed.count() < 1.0);
}

TEST_CASE("parsing: < and <= between nested terms are not read as a list first")
{
    // The same duplication occurring through an alternate entry point. "<"
    // also opens a node list, < a b >, and the grammar first tried
    // interpreting it this way: the effort to parse the rest of the
    // statement, including groups, ended in failure because of the missing
    // closing ">", leading to a renewed reading of the statement where "<"
    // functions as the predicate. Thus, (a < (a < ... x)) p q underwent
    // doubling at every level, just as <= did, since both begin with the
    // same character -- 12 s and 2 GB at depth 22 after groups were read
    // only once. A list is now considered exclusively at positions where ">"
    // concludes it before the enclosing group ends. Both are real
    // predicates: integer comparison produces facts via <, and rules utilize
    // it for comparison.
    //
    // A test limited by time, as justified by the rationale given in the
    // previous test. Depth 22 instead of 20: at depth 20 the line required
    // approximately 3 s without the probe, nearing the bound too closely to
    // be sure of failing on a fast machine.
    for (const std::string op : {"<", "<="})
    {
        CAPTURE(op);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        std::string term = "x";
        for (int depth = 0; depth < 22; ++depth)
            term = "(a " + op + " " + term + ")";

        const auto start = std::chrono::steady_clock::now();
        interactive.process(term + " p q");
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        CHECK(any_output_contains(collector, term + " p q"));
        CHECK(elapsed.count() < 1.0);
    }

    // A node list keeps its reading, positioned adjacent to < as a
    // predicate on a single line.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("<a b c> r (x < y)");
        collector.clear();
        interactive.process("<a b c> r Y");
        CHECK(answers_contain(collector, "<a b c> r (x < y)")); });
}

TEST_CASE("parsing: the list probe refuses a bracket that does not close inside it")
{
    // The probe skipped balanced groups and sets, yet encountered a "(" or
    // "{" that remained unclosed within it, treating it as a plain
    // character. A line where brackets balance in number but differ in
    // type consequently passed the probe at every level: each level tried
    // the list, failed, and proceeded to read the remainder again,
    // interpreting "<" as an operator. All three lines below remain
    // unparsable either way; determining this took roughly 10 s
    // for the first at depth 24 and 8 s for the second at depth 26. The
    // third line is the set counterpart of the second, a run
    // of "{" where the innermost set closes with a ")", and required a
    // similar duration. A group or set that successfully parses is
    // balanced, hence the probe now refuses such a bracket.
    //
    // A limit on time, similar to the tests shown earlier.
    // Since no line costs memory, the depths can be
    // selected for a wide margin.
    std::string chain = "x";
    for (int depth = 0; depth < 24; ++depth)
        chain = "(a < " + chain + ")";
    chain += " p q";
    chain[chain.find(')')] = '}'; // the most deeply nested group closes with an incorrect bracket

    const std::string open_run  = "a p < " + std::string(26, '(') + "} " + std::string(25, ')');
    const std::string brace_run = "a p < " + std::string(26, '{') + ") " + std::string(25, '}');

    for (const std::string& line : {chain, open_run, brace_run})
    {
        CAPTURE(line);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const auto start = std::chrono::steady_clock::now();
        CHECK_THROWS_AS(interactive.process(line), std::runtime_error);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        CHECK(elapsed.count() < 1.0);
    }
}

TEST_CASE("parsing: the list probe stops at a comma between two conditions")
{
    // The expression (x < a, b > c) holds two comparisons, yet the probe
    // from the "<" discovered the ">" situated after the comma. The list
    // attempt proceeded to read the rest of the group, including a nested
    // one, until it encountered the comma and failed. Subsequently,
    // :stmt-any re-read the entire segment with "<" serving as the operator,
    // consuming 5 seconds and 800 MB at depth 20. A node list never holds a
    // comma that separates conditions, hence the probe now stops there.
    //
    // A time bound, similar to the tests
    // mentioned above.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    std::string term = "y";
    for (int depth = 0; depth < 20; ++depth)
        term = "(x < " + term + ", b > c)";

    const auto start = std::chrono::steady_clock::now();
    interactive.process(term + " => (x p q)");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    // The echo prints the conditions according to node order, thus it is
    // not the input once more; the innermost comparison and the conclusion
    // show the reading.
    CHECK(any_output_contains(collector, "(x < y)"));
    CHECK(any_output_contains(collector, "=> (x p q)"));
    CHECK(elapsed.count() < 1.0);
}

TEST_CASE("parsing: the list probe skips a node list nested in the probed one")
{
    // A node list after "<", as in (a < <b c> T), gave the probe a ">" to
    // find: the one that closes <b c>. Thus, the list attempt read T,
    // encountered failure at the ")" marking the group, and :stmt-any read T
    // again with "<" functioning as an operator -- 6 seconds and 1.1 GB at
    // depth 20, with similar consumption for the list following T. zelph
    // prints such expressions autonomously, so its own output re-entered
    // that slowly. The probe now counts every "<" it passes, and
    // the ">" of a nested list now terminates that nested list rather than
    // the one being probed.
    //
    // A time bound, similar to the tests
    // mentioned above.
    for (const std::string shape : {"(a < <b c> T)", "(a < T <b c>)"})
    {
        CAPTURE(shape);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const std::size_t hole = shape.find('T');
        std::string       term = "x";
        for (int depth = 0; depth < 20; ++depth)
            term = shape.substr(0, hole) + term + shape.substr(hole + 1);

        const auto start = std::chrono::steady_clock::now();
        interactive.process(term + " p q");
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        // The echo outputs the two objects of each group in node order,
        // thus it is not the input once more; each <b c> within it shows
        // the list reading.
        const std::string echo  = normalize(last_out_text(collector));
        std::size_t       lists = 0;
        for (std::size_t at = echo.find("<b c>"); at != std::string::npos; at = echo.find("<b c>", at + 1))
            ++lists;
        CHECK(lists == 20);
        CHECK(echo.find(") p q") != std::string::npos);
        CHECK(elapsed.count() < 1.0);
    }

    // The probe counts rather than diving deeper via each "<": a
    // recursive probe goes one level deeper per "<", and on this line, 400
    // operators in length, it exceeded the recursion limit of peg/match,
    // which refused the line.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string line = "x p";
        for (int i = 0; i < 400; ++i)
            line += " a <";
        CHECK_NOTHROW(interactive.process(line + " b"));
        collector.clear();
        interactive.process("x p W");
        CHECK(answers_contain(collector, "x p <"));
        CHECK(answers_contain(collector, "x p b")); });
}

TEST_CASE("parsing: the list probe skips an arrow that begins a value")
{
    // The arrows ->, --> and => hold a ">", and the probe originating from
    // a "<" took that ">" for the termination point of the list. Thus,
    // in (a < -> T) the list attempt read T, encountered failure at the ")"
    // marking the end of the group, and :stmt-any re-read T using "<" as
    // the operator -- 4 to 8 s at depth 20 for each shape below, regardless
    // of whether the arrow precedes or follows T. A value commencing with
    // one of these arrows is read as the arrow itself, so the probe now
    // skips an arrow wherever the reading process is positioned at the
    // start of a value: after a blank that cannot be part of an atom, and
    // after a string, a group, a set, a list, or another arrow. A form feed
    // also serves to separate values in such contexts.
    //
    // In the last four shapes, a form feed stands right before the last
    // arrow, ensuring the arrow does not come immediately after a blank,
    // and each shape holds its own skip: x \f-> represents the arrow behind
    // a blank and a form feed, "q"\f-> denotes the arrow behind a string,
    // <b c>\f-> signifies the arrow behind a nested list, and ->\f->
    // indicates the arrow behind another arrow. Without the skip it holds,
    // a shape doubles per level again: x \f-> and "q"\f-> took 7 to 9 s,
    // while the probe skipped an arrow only when it was directly behind a
    // blank.
    //
    // A time bound, similar to the tests
    // above.
    for (const auto& [shape, arrow] : {std::pair<std::string, std::string>{"(a < -> T)", "->"},
                                       {"(a <= => T)", "=>"},
                                       {"(a < T ->)", "->"},
                                       {"(a < --> T)", "-->"},
                                       {"(a < x \f-> T)", "->"},
                                       {"(a < \"q\"\f-> T)", "->"},
                                       {"(a < <b c>\f-> T)", "->"},
                                       {"(a < ->\f-> T)", "->"}})
    {
        CAPTURE(shape);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const std::size_t hole = shape.find('T');
        std::string       term = "x";
        for (int depth = 0; depth < 20; ++depth)
            term = shape.substr(0, hole) + term + shape.substr(hole + 1);

        const auto start = std::chrono::steady_clock::now();
        interactive.process(term + " p q");
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        // The echo outputs the objects of each group according to node
        // order, thus it does not always reproduce the input; it holds a
        // single arrow per group, as the two arrows in ->\f-> constitute a
        // single node.
        const std::string echo   = normalize(last_out_text(collector));
        std::size_t       arrows = 0;
        for (std::size_t at = echo.find(arrow); at != std::string::npos; at = echo.find(arrow, at + 1))
            ++arrows;
        CHECK(arrows == 20);
        CHECK(echo.find(") p q") != std::string::npos);
        CHECK(elapsed.count() < 1.0);
    }

    // A form feed constitutes whitespace separating values, yet an atom
    // consumes one that follows it: within this line, a\f- is an atom
    // and the ">" closes the list. A probe that skipped an arrow after any
    // whitespace skipped \f-> as well, found no ">", and refused a line
    // that parses.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_NOTHROW(interactive.process("y q < a\f->"));
        collector.clear();
        interactive.process("X q Y");
        CHECK(answers_contain(collector, "y q < \"a\f-\" >")); });
}

TEST_CASE("parsing: the list probe reads a run of blanks once")
{
    // The probe looks for an arrow positioned behind every run of blank
    // characters. As it processed the remaining portion of each run again
    // from every blank it traversed, detecting no arrow each time, a run of
    // n blanks incurred a cost of n²: this valid line took 7 to 8 s,
    // compared to less than 0.01 s when using a probe that did not look for
    // arrows. The probe now reads such a run in full, regardless of whether
    // an arrow appears afterwards.
    //
    // A time bound, similar to the tests
    // above.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto start = std::chrono::steady_clock::now();
    interactive.process("x p < " + std::string(64000, ' ') + "b c>");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed.count() < 1.0);
    collector.clear();
    interactive.process("x p Y");
    CHECK(answers_contain(collector, "x p <b c>"));
}

TEST_CASE("parsing: the list probe skips the operand of a prefix form")
{
    // A prefix form reads its operand as a value, and an operand commencing
    // with a ">" carries it along: in ¬-> the ¬ negates the arrow ->, in
    // :p > the self-fact sugar holds the atom >, and in :p-> the name
    // becomes p- while the operand is the atom > once more. The probe
    // originating from a "<" took that ">" for the end of the list. Thus in
    // (a < ¬-> T) the list attempt read T, failed upon encountering the ")"
    // of the group, and :stmt-any read T again with "<" serving as the
    // operator -- 4 to 6 s and 0.8 to 0.9 GB at depth 20 for each of the
    // first five shapes below, with time and memory doubling at each level.
    // Where a value starts, the probe now skips a run of prefixes and the
    // arrow, ">=" or ">" that the operand begins with.
    //
    // The next four shapes put the prefix where the initial five are unable
    // to. In the first three, it resides immediately after the "<" of a
    // nested list, a location where a value also commences, or it is glued
    // to an atom inside a nested list, where ¬ and * start a value of their
    // own because both are reserved; these required 10 to 12 s and 1.3 to
    // 1.5 GB at depth 20. In the fourth, it is glued to the "<" from which
    // the probe originates, and this shape required 4 to 5 s and 0.8 GB at
    // depth 20. That "<" does not represent a list, as :p claims the ">",
    // and it remains a member of the set. A set is essential here: within a
    // group, a "<" glued to what comes after is not a predicate, and the
    // line lacks a reading. The four shapes that follow place the prefix
    // after something the probe has just read, where a value starts again:
    // after the blanks following an atom, after a string, which the probe
    // reads like a group, a set, or a compact list, after the ">" of a
    // nested list, and after a * glued to an atom inside a nested list; a
    // blank comes after that *, making it the atom *. The probe skips a
    // prefix at each of these positions individually, and each of the four
    // shapes took 5 to 14 s and 1.2 to 1.9 GB at depth 20 when the skip at
    // its respective location was absent. The final two shapes hold what
    // the probe reads alongside a prefix. A ¬ reads the blanks preceding
    // its operand, so in ¬ > the ">" serves as that operand, and the probe
    // skips those blanks in tandem with the ¬. In :q >=:p > the operand of
    // :q is >=, and :p > comes after it without a blank in between. The
    // probe skips the >= as a whole, thus standing at the start of a value
    // where :p begins; a probe that skipped only the ">" of >= read the "="
    // as a plain character, and the ">" of :p > ended the list. Each of the
    // two took 4 to 8 s at depth 20 while the probe was missing the
    // component it holds.
    //
    // ¬ and ≈ operate on an entire rule condition and on nothing contained
    // within it, thus a line containing them here is refused -- following
    // the parse, which is what the time bound measures, and before anything
    // is written. The remaining lines consist of statements, and their echo
    // shows the reading: a group prints its objects and a set lists its
    // members in node order, with each self-fact or list appearing once per
    // level, and * makes each group represent its focus, so the line with
    // *-> transforms into -> p q.
    //
    // A time bound, similar to the tests
    // above.
    const std::string negation = "\"¬\" is a condition operator";
    const std::string approx   = "\"≈\" is a condition operator";
    const std::string none;

    // shape, the refusal it meets (none for a statement), and what its echo
    // holds once per level
    for (const auto& [shape, refusal, per_level] : {std::tuple<std::string, std::string, std::string>{"(a < ¬-> T)", negation, ""},
                                                    {"(a < *-> T)", none, ""},
                                                    {"(a < :p-> T)", none, "(:p- >)"},
                                                    {"(a < :p > T)", none, "(:p >)"},
                                                    {"(a < ≈n-> T)", approx, ""},
                                                    {"(a < <:p > b> T)", none, "<(:p >) b>"},
                                                    {"(a < <b x¬-> c> T)", negation, ""},
                                                    {"(a < <b x*-> c> T)", none, "->"},
                                                    {"{a <:p > T}", none, "(:p >)"},
                                                    {"(a < b :p > T)", none, "(:p >)"},
                                                    {"(a < \"q\" :p > T)", none, "(:p >)"},
                                                    {"(a < <b c> :p > T)", none, "(:p >)"},
                                                    {"(a < <b x* :p > c> T)", none, "(:p >)"},
                                                    {"(a < ¬ > T)", negation, ""},
                                                    {"{a < :q >=:p > T}", none, "(:q >=)"}})
    {
        CAPTURE(shape);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const std::size_t hole = shape.find('T');
        std::string       term = "x";
        for (int depth = 0; depth < 20; ++depth)
            term = shape.substr(0, hole) + term + shape.substr(hole + 1);

        const auto start = std::chrono::steady_clock::now();
        if (refusal.empty())
        {
            CHECK_NOTHROW(interactive.process(term + " p q"));
        }
        else
        {
            CHECK_THROWS_WITH_AS(interactive.process(term + " p q"), doctest::Contains(refusal), std::runtime_error);
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        CHECK(elapsed.count() < 1.0);
        if (!refusal.empty())
            continue;

        const std::string echo = normalize(last_out_text(collector));
        if (per_level.empty())
        {
            CHECK(echo == "-> p q");
            collector.clear();
            interactive.process("a < -> X");
            CHECK(answers_contain(collector, "a < -> x"));
            continue;
        }
        std::size_t levels = 0;
        for (std::size_t at = echo.find(per_level); at != std::string::npos; at = echo.find(per_level, at + 1))
            ++levels;
        CHECK(levels == 20);
        CHECK(echo.find(shape.substr(hole + 1) + " p q") != std::string::npos);
    }

    // Where the probe must not skip. * reads its operand without a blank
    // intervening, so a form feed immediately following it begins an atom:
    // on the first line \f- constitutes that atom, and the list represents
    // its focus. A * followed directly by a blank forms the atom *, as seen
    // in the second line. A ":" or "≈" embedded within an atom does not
    // trigger a prefix: in the third and fourth lines, x:p and x≈n are
    // atoms. Nor does a ":" or "≈" lacking a name after it: in the last two
    // lines, it is an atom of its own. In every line, the ">" closes the
    // list, and a probe that skips it finds no other and refuses a line
    // that parses.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_NOTHROW(interactive.process("y q <a *\f->"));
        CHECK_NOTHROW(interactive.process("y q <a * >"));
        CHECK_NOTHROW(interactive.process("y q <b x:p >"));
        CHECK_NOTHROW(interactive.process("y q <b x≈n >"));
        CHECK_NOTHROW(interactive.process("y q <a : >"));
        CHECK_NOTHROW(interactive.process("y q <a ≈ >"));
        collector.clear();
        interactive.process("y q X");
        CHECK(answers_contain(collector, "y q \"\f-\""));
        CHECK(answers_contain(collector, "y q <b x:p>"));
        CHECK(answers_contain(collector, "y q <b x≈n>"));
        CHECK(answers_contain(collector, "y q <a \":\">"));
        CHECK(answers_contain(collector, "y q <a \"≈\">"));

        // The second line is verified using the quoted spelling of the
        // atom * instead of comparing it to a printed answer, meaning the
        // check is unaffected by how the printer spells a bare * in a list:
        // y answers next to y2 only when both lines hold the identical list.
        interactive.process("y2 q <a \"*\">");
        collector.clear();
        interactive.process("X q <a \"*\">");
        const std::vector<std::string> answers = collect_answers(collector);
        CHECK(answers.size() == 2);
        CHECK(std::any_of(answers.begin(), answers.end(), [](const std::string& a)
                          { return a.rfind("y q ", 0) == 0; }));
        CHECK(std::any_of(answers.begin(), answers.end(), [](const std::string& a)
                          { return a.rfind("y2 q ", 0) == 0; })); });
}

TEST_CASE("parsing: a prefix whose operand fails is not read again as an atom")
{
    // :pred X, ≈net X, *X, and @{...} are each evaluated before considering
    // the atom that the prefix might also be read as. If the operand failed
    // to parse, the prefix was treated as an atom instead, leaving the same
    // operand for the next value. That value then processed it -- and every
    // prefix form contained within it -- a second time, resulting in the same
    // failure, since the line lacks any valid reading. Consequently, a
    // MALFORMED line incurred double the cost per nesting level,
    // approximately half a second at depth 18 for each of the four forms;
    // well-formed input never follows this route. The atom is now rejected
    // when no content after the prefix can end a value.
    //
    // A time bound, similar to the nesting tests above.
    for (const std::string shape : {"(:p T)", "(≈n T)", "{*T}", "{@{T}}"})
    {
        CAPTURE(shape);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const std::size_t hole = shape.find('T');
        std::string       term = "()";
        for (int depth = 0; depth < 22; ++depth)
            term = shape.substr(0, hole) + term + shape.substr(hole + 1);

        const auto start = std::chrono::steady_clock::now();
        CHECK_THROWS_AS(interactive.process("a p " + term), std::runtime_error);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        CHECK(elapsed.count() < 1.0);
    }

    // When a value concludes immediately following the prefix, the prefix
    // continues to be interpreted as an atom.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::pair<std::string, std::string> cases[] = {
            {"{a :p} r b", R"({a ":p"} r b)"},
            {"{a ≈n} r b", R"({a "≈n"} r b)"},
            {"{a *} r b", "{a *} r b"},
            {"{@ {a}} r b", "{@ {a}} r b"}};
        for (const auto& [line, echo] : cases)
        {
            CAPTURE(line);
            collector.clear();
            interactive.process(line);
            CHECK(any_output_starts_with(collector, echo));
        } });
}

TEST_CASE("parsing: a node list whose content fails is not read again with < as an operator")
{
    // The identical doubling via a node list. :list-probe found the closing
    // ">", the list's content failed due to a malformed operand, and "<"
    // was subsequently interpreted as an operator: the atom "<" left the
    // same content to the next value, which failed the same way. Each level
    // doubled the workload, consuming several seconds at depth 22 for each
    // of the two shapes. When the content stops at a position that serves
    // neither to close the list nor to terminate a value, no reading
    // exists, and the parse ceases at that point.
    //
    // A time bound, similar to the nesting tests previously shown.
    for (const std::string shape : {"<a T>", "<*T >"})
    {
        CAPTURE(shape);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        const std::size_t hole = shape.find('T');
        std::string       term = "()";
        for (int depth = 0; depth < 22; ++depth)
            term = shape.substr(0, hole) + term + shape.substr(hole + 1);

        const auto start = std::chrono::steady_clock::now();
        CHECK_THROWS_AS(interactive.process("a p " + term), std::runtime_error);
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

        CHECK(elapsed.count() < 1.0);
    }

    // When the content stops at a comma separator, "<" keeps its reading as
    // an operator: two distinct comparisons, not a list.
    //
    // The same applies when the content stops at ")", "}" or the end of the
    // line. In :p > the ">" functions as the operand within self-fact
    // sugar, meaning none of the three subsequent lines contains a list
    // capable of closing, and each must read "<" as an operator: aborting
    // the parse where a list attempt stopped at one of these three refused
    // a line that has a reading. The probe skips such an operand and
    // refuses the list before it is read; a list attempt that stops at one
    // of the three still reverts to the operator.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(x < a, b > c) => (x p q)");
        interactive.process("x < a");
        interactive.process("b > c");
        collector.clear();
        interactive.process("x p Q");
        CHECK(answers_contain(collector, "x p q"));

        CHECK_NOTHROW(interactive.process("a < :p >"));
        collector.clear();
        interactive.process("a < Q");
        CHECK(answers_contain(collector, "a < (:p >)"));

        CHECK_NOTHROW(interactive.process("(a < :p >) r s"));
        collector.clear();
        interactive.process("X r s");
        CHECK(answers_contain(collector, "(a < (:p >)) r s"));

        // A set prints its members according to node order, hence no
        // exact answer.
        CHECK_NOTHROW(interactive.process("{a < :p >} t s"));
        collector.clear();
        interactive.process("X t s");
        const auto answers = collect_answers(collector);
        CHECK(std::ranges::any_of(answers, [](const std::string& a)
                                  { return a.starts_with("{") && a.ends_with("} t s") && a.find("(:p >)") != std::string::npos; })); });
}

TEST_CASE("parsing: set with facts")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "set_logic ~ { (myItem1 IsA myItem2) (myItem2 IsA myItem3) }");
        CHECK(any_output_contains(collector, "myItem1 IsA myItem2"));
        CHECK(any_output_contains(collector, "myItem2 IsA myItem3")); });
}

// ---------------------------------------------------------------------------
// Focus operator and variable queries
// ---------------------------------------------------------------------------

TEST_CASE("focus operator and variable query")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(*tim ~ human) ~ male
tim _predicate _object
)");
        CHECK(any_output_starts_with(collector, "tim ~ male"));
        CHECK(answers_contain(collector, "tim ~ human"));
        CHECK(answers_contain(collector, "tim ~ male")); });
}

// ---------------------------------------------------------------------------
// Nested unification
// ---------------------------------------------------------------------------

TEST_CASE("nested unification: pattern matching in equations")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
((A + B) = C) => (test A B)
(4 + 5) = 9
)");
        CHECK(any_output_starts_with(collector, "( test 4 5 )")); });
}

TEST_CASE("nested unification: deep structure matching")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(subj pred (obj is (subj2 A (b test C)))) => (success A C)
subj pred (obj is (subj2 a_val (b test c_val)))
)");
        CHECK(any_output_starts_with(collector, "( success a_val c_val )")); });
}

// ---------------------------------------------------------------------------
// Complex conjunction rule
// ---------------------------------------------------------------------------

TEST_CASE("complex conjunction rule with followed-by")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
((A + B) = C) => (test A B)
(4 + 5) = 9
(*{ ((A + B) = C) (B followed-by D) (C followed-by E) } ~ conjunction) => ((A + D) = E)
5 followed-by 42
9 followed-by 43
)");
        CHECK(any_output_starts_with(collector, "(( 4 + 42 ) = 43 )")); });
}

// ---------------------------------------------------------------------------
// Peano-style rule
// ---------------------------------------------------------------------------

TEST_CASE("peano-style successor rule")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(A followed-by B) => ((<1> + A) = B)
<0> followed-by <1>
)");
        CHECK(any_output_starts_with(collector, "((<1> + <0>) = <1>)")); });
}

// ---------------------------------------------------------------------------
// Negation
// ---------------------------------------------------------------------------

TEST_CASE("negation: last element of list")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
elem1 --> elem2
elem2 --> elem3
elem3 --> elem4
elem4 --> elem5
elem1 partoflist mylist
elem2 partoflist mylist
elem3 partoflist mylist
elem4 partoflist mylist
elem5 partoflist mylist
(A partoflist L, *(A --> X) ~ negation) => (A "is last of" L)
)");
        CHECK(any_output_starts_with(collector, "( elem5 \"is last of\" mylist )")); });
}

TEST_CASE("negation: syntax sugar with not-green rule")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(A is yellow, ¬(A is green)) => (A "is not" green)
plant is green
plant is yellow
plant2 is yellow
)");
        // plant is both yellow and green, so rule does not fire for plant.
        // plant2 is yellow but not green, so the rule fires.
        CHECK(any_output_starts_with(collector, "( plant2 \"is not\" green )")); });
}

// ---------------------------------------------------------------------------
// Contradiction detection
// ---------------------------------------------------------------------------

TEST_CASE("contradiction detection")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(A instanceof B, A subclassof B) => !
gene instanceof geneclass
gene subclassof geneclass
)");
        CHECK(any_output_starts_with(collector, "!"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("naming: core-name merge via .name does not deadlock")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
.name ! en contradiction
! P O
)");

        CHECK(answers_contain(collector, "! is unsatisfiable"));
        CHECK_FALSE(any_event_contains(collector, "Resource deadlock avoided")); });
}

TEST_CASE("naming: repeated .name assignment stays stable")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
.name ! en contradiction
.name ! en contradiction
! P O
)");

        CHECK(answers_contain(collector, "! is unsatisfiable"));
        CHECK_FALSE(any_event_contains(collector, "Resource deadlock avoided")); });
}

// ---------------------------------------------------------------------------
// Janet integration
// ---------------------------------------------------------------------------

TEST_CASE("janet: inline fact and multiline block with deduction")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
%(zelph/fact "Berlin" "is capital of" "Germany")
Germany "is located in" Europe
%
(let [cond (zelph/set
            (zelph/fact 'X "is capital of" 'Y)
            (zelph/fact 'Y "is located in" 'Z))]
(zelph/fact cond "~" "conjunction")
(zelph/fact cond "=>" (zelph/fact 'X "is located in" 'Z)))
%
)");
        CHECK(any_output_starts_with(collector, "( Berlin \"is located in\" Europe )")); });
}

TEST_CASE("janet: unquote referencing janet variable")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
%(def berlin (zelph/resolve "Berlin"))
,berlin ~ town
)");
        CHECK(any_output_starts_with(collector, "Berlin ~ town")); });
}

// ---------------------------------------------------------------------------
// Transitive relation deduction
// ---------------------------------------------------------------------------

TEST_CASE("transitive relation deduction")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(R is transitive, A R B, B R C) => (A R C)
6 > 5
5 > 4
> is transitive
)");
        CHECK(any_output_starts_with(collector, "( 6 > 4 )")); });
}

// ---------------------------------------------------------------------------
// Inequality (!=) semantics
//
// Core design decision: different variable names do NOT imply inequality.
// Variables X and Y may bind to the same node unless an explicit X != Y
// constraint is present.  != is a built-in guard (not a fact lookup) that
// filters bindings after the involved variables are bound.
// ---------------------------------------------------------------------------

TEST_CASE("inequality: different variable names may bind to the same value")
{
    // Without !=, two distinct variables can unify with the same node.
    // Rule: if A has property X and property Y, derive has_pair.
    // With only one value "v", X and Y should both bind to "v".
    // Note: objects are stored as adjacency_set, so {v, v} collapses to {v}.
    // The key assertion is that the rule fires at all with only one value.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y) => (A has_pair X Y)
a prop v
)");
        CHECK(any_output_starts_with(collector, "( a has_pair v )")); });
}

// ---------------------------------------------------------------------------
// Two patterns that a self-join makes necessary, both documented in logic.md
// and neither pinned until now.
//
// The first is a performance rewrite, and what has to hold is that it is ONLY
// a performance rewrite: pushing a selection below a self-join must derive
// exactly the same facts. The advice is worthless if the two forms can differ.
//
// The second answers "is this the ONLY one", which looks as if it needs a
// negation over a conjunction - the non-existent `not (A prop Y, X != Y)`.
// It does not: derive the positive "there are two" with the guard, then negate
// THAT single pattern. Stratified negation evaluates it against the saturated
// positive base, which is exactly when the marker is complete.
// ---------------------------------------------------------------------------

// Disjunction has no syntax in a rule body; logic.md says to express it as
// several rules sharing one consequence, and that was untested. What has to
// hold is that either branch alone suffices, that both together derive the
// same fact rather than two, and that the head does not appear from nothing.
TEST_CASE("disjunction: several rules, one consequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A ~ bird) => (A can fly)
(A ~ bat) => (A can fly)
tweety ~ bird
bruce ~ bat
rex ~ lizard
)");
        CHECK(any_output_starts_with(collector, "( tweety can fly )"));
        CHECK(any_output_starts_with(collector, "( bruce can fly )"));
        CHECK_FALSE(any_output_starts_with(collector, "( rex can fly )")); });
}

TEST_CASE("disjunction: both branches true derive one fact, not two")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A ~ bird) => (A can fly)
(A ~ bat) => (A can fly)
oddity ~ bird
oddity ~ bat
X can fly
)");
        // A fact is a node and nodes are hash-consed, so the second branch
        // finds the fact the first one made rather than making another. The
        // query therefore answers exactly once.
        CHECK(count_outputs_starting_with(collector, "Answer: oddity can fly") == 1); });
}

TEST_CASE("self-join: deriving the selection first changes nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A hits B, A hits C, B != C, B holds K, K ~ valuable, C holds L, L ~ valuable) => (A ~ direct)
(A hits B, B holds K, K ~ valuable) => (A threatens B)
(A threatens B, A threatens C, B != C) => (A ~ pushed)
n hits e5
n hits c7
n hits a1
e5 holds queen
c7 holds rook
a1 holds pawn
queen ~ valuable
rook ~ valuable
)");
        // Both forms fire, on the same subject, and the pawn on a1 - which is
        // not `valuable` - is what would make a careless rewrite differ.
        CHECK(any_output_starts_with(collector, "( n ~ direct )"));
        CHECK(any_output_starts_with(collector, "( n ~ pushed )")); });
}

TEST_CASE("self-join: the pushed form does not fire where the direct one cannot")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A hits B, A hits C, B != C, B holds K, K ~ valuable, C holds L, L ~ valuable) => (A ~ direct)
(A hits B, B holds K, K ~ valuable) => (A threatens B)
(A threatens B, A threatens C, B != C) => (A ~ pushed)
n hits e5
n hits a1
e5 holds queen
a1 holds pawn
queen ~ valuable
)");
        // One valuable victim only: neither form may fire, and in particular
        // the `!=` guard must not pair the single threat with itself.
        CHECK_FALSE(any_output_starts_with(collector, "( n ~ direct )"));
        CHECK_FALSE(any_output_starts_with(collector, "( n ~ pushed )")); });
}

TEST_CASE("negation: `the only one` needs a derived marker, not a negated conjunction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a prop v1
a prop v2
b prop w
(A prop X, A prop Y, X != Y) => (A ~ several)
(A prop X, ¬(A ~ several)) => (X ~ sole)
)");
        // `b` has exactly one, `a` has two. The marker is derived first and
        // the negation is then a single pattern, which `not` can express.
        CHECK(any_output_starts_with(collector, "( w ~ sole )"));
        CHECK_FALSE(any_output_starts_with(collector, "( v1 ~ sole )"));
        CHECK_FALSE(any_output_starts_with(collector, "( v2 ~ sole )")); });
}

TEST_CASE("inequality: != prevents same-value binding")
{
    // Same setup, but X != Y blocks the (v, v) binding.
    // With only one value, the rule must NOT fire at all.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y, X != Y) => (A has_pair X Y)
a prop v
)");
        CHECK_FALSE(any_output_starts_with(collector, "( a has_pair")); });
}

TEST_CASE("inequality: != allows binding when values differ")
{
    // Two different values exist. != should allow the pairs where X != Y.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y, X != Y) => (A has_pair X Y)
a prop v1
a prop v2
)");
        CHECK(any_output_starts_with(collector, "( a has_pair"));
        // Both orderings may appear (objects are a set, so {v1,v2} = {v2,v1}):
        bool has_v1_v2 = any_output_starts_with(collector, "( a has_pair v1 v2 )") ||
                         any_output_starts_with(collector, "( a has_pair v2 v1 )");
        CHECK(has_v1_v2); });
}

TEST_CASE("inequality: contradiction with != (opposite scenario from log)")
{
    // The exact scenario from the bug report: != should not break
    // contradiction detection.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y, X != Y) => !
bright opposite dark
yellow ~ bright
yellow ~ dark
)");
        CHECK(any_output_starts_with(collector, "!"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: contradiction without != when data forces distinct bindings")
{
    // Without !=, X and Y CAN bind to the same value in principle.
    // However, here the only existing "opposite" fact is (bright opposite dark),
    // so the unification forces X=bright, Y=dark — they happen to be distinct
    // because of the data, not because of an implicit inequality constraint.
    // This test verifies that the engine still finds the contradiction in
    // this data-driven scenario.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y) => !
bright opposite dark
yellow ~ bright
yellow ~ dark
)");
        CHECK(any_output_starts_with(collector, "!"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: != with ground constants is trivially true")
{
    // When both sides are ground and unequal, != succeeds immediately.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A likes B, a != b) => (A taste diverse)
joe likes pizza
)");
        CHECK(any_output_starts_with(collector, "( joe taste diverse )")); });
}

TEST_CASE("inequality: != with identical ground constants blocks rule")
{
    // When both sides are ground and equal, != must block the rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A likes B, a != a) => (A taste diverse)
joe likes pizza
)");
        CHECK_FALSE(any_output_starts_with(collector, "( joe taste diverse )")); });
}

TEST_CASE("inequality: a structured operand is resolved, not compared as a pattern")
{
    // A != operand may be a plain variable, a ground node, or a STRUCTURED
    // PATTERN. The pattern case used to be treated as ground: the
    // comparison ran against the pattern node itself, which is never
    // identical to a concrete argument, so the guard silently permitted
    // everything -- the worst failure mode for a guard, since it reads
    // correctly. (A cons R) != &0 is the natural way to write "this
    // numeral is not zero", and the standard library needs it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");
        process_lines(interactive, R"(
((A cons R) probe M, (A cons R) != &0) => ((A cons R) nonzero M)
&0 probe t
&5 probe t
)");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "UNEQ-KEEP-" (zelph/exists (zelph/number "5") "nonzero" "t")))js");
        interactive.process(R"js(%(string "UNEQ-BLOCK-" (zelph/exists (zelph/number "0") "nonzero" "t")))js");
        CHECK(any_output_contains(collector, "UNEQ-KEEP-true"));
        CHECK(any_output_contains(collector, "UNEQ-BLOCK-false")); });
}

TEST_CASE("inequality: a structured operand whose variables bind later still blocks")
{
    // Both call sites must agree: the immediate check in evaluate() and the
    // deferred one in contradicts(). Ordering the guard BEFORE the
    // condition that binds its variables exercises the deferred path.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");
        process_lines(interactive, R"(
((A cons R) != &0, (A cons R) probe M) => ((A cons R) late M)
&0 probe t
&5 probe t
)");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "UNEQ-LATE-KEEP-" (zelph/exists (zelph/number "5") "late" "t")))js");
        interactive.process(R"js(%(string "UNEQ-LATE-BLOCK-" (zelph/exists (zelph/number "0") "late" "t")))js");
        CHECK(any_output_contains(collector, "UNEQ-LATE-KEEP-true"));
        CHECK(any_output_contains(collector, "UNEQ-LATE-BLOCK-false")); });
}

TEST_CASE("inequality: a structured operand denoting no existing fact does not block")
{
    // Resolve::Missing -- every variable bound, but the denoted fact is not
    // in the graph. It cannot be the node the other side is bound to, so
    // the guard must pass rather than block.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X pairs Y, (X op Y) != absent) => (X ok Y)
a pairs b
)");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "UNEQ-MISSING-" (zelph/exists "a" "ok" "b")))js");
        CHECK(any_output_contains(collector, "UNEQ-MISSING-true")); });
}

TEST_CASE("inequality: a guard that never gets its bindings is not a match")
{
    // logic.md states the contract: != is a guard constraint, NOT a fact
    // lookup, and it filters variable bindings AFTER the involved variables
    // are bound by positive conditions. With nothing bound there is nothing
    // to filter -- but the guard used to succeed vacuously, so a query
    // answered its own pattern with the variables still unbound:
    //
    //     zelph> S != O
    //     Answer: S != O          <- on an EMPTY network, too
    //
    // Deferring an undecidable guard stays right while conditions are being
    // joined; the check belongs at the terminal point, where no binding can
    // arrive any more. See Reasoning::guards_unresolved.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("S != O");
        CHECK(collect_answers(collector).empty());

        interactive.process("a rel b");

        // One side bound is no better: the other one still filtered nothing.
        collector.clear();
        interactive.process("a != O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S != b");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("inequality: a guard naming a variable no condition binds blocks the rule")
{
    // The rule-level half of the same contract, and the behaviour change it
    // implies: Z is bound by no positive condition, so the guard never
    // filters anything and the rule must not fire on it. It used to fire,
    // because an unresolvable guard was skipped as "not contradicting".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X rel Y, Y != Z) => (X differs Y)
a rel b
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S differs O");
        CHECK(collect_answers(collector).empty());

        // The same rule with a guard both of whose sides the conditions bind
        // fires as before -- this is the control that the terminal check did
        // not simply disable the guard.
        process_lines(interactive, R"(
(X rel Y, X != Y) => (X apart Y)
c rel c
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S apart O");
        CHECK(answers_contain(collector, "a apart b"));
        CHECK_FALSE(answers_contain(collector, "c apart c")); });
}

TEST_CASE("inequality: reflexive opposite without != causes false positive")
{
    // KEY MOTIVATION for !=:
    // If "opposite" includes a reflexive fact (bright opposite bright),
    // then without != the rule fires with X=Y=bright, A=yellow,
    // which is a spurious contradiction (yellow is bright AND bright,
    // but those are the same thing — not a real conflict).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y) => !
bright opposite bright
yellow ~ bright
)");
        // Without !=, this DOES fire — it's a false positive.
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: reflexive opposite with != prevents false positive")
{
    // Same scenario, but with != the X=Y=bright binding is blocked.
    // No contradiction should be found.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y, X != Y) => !
bright opposite bright
yellow ~ bright
)");
        CHECK_FALSE(has_contradiction(collector)); });
}

TEST_CASE("inequality: transitive rule with != prevents trivial self-deduction")
{
    // Without !=, (a > b, b > a) would derive a > a.  With != this is blocked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is transitive_strict, A R B, B R C, A != C) => (A R C)
a > b
b > a
> is transitive_strict
)");
        // a > b > a should NOT produce a > a
        CHECK_FALSE(any_output_starts_with(collector, "( a > a )"));
        CHECK_FALSE(any_output_starts_with(collector, "( b > b )")); });
}

TEST_CASE("inequality: multiple != constraints in one rule")
{
    // All three variables must be pairwise distinct.
    // Use logged mode to help debug if this fails.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(X member group, Y member group, Z member group, X != Y, Y != Z, X != Z) => (triple X Y Z)
a member group
b member group
c member group
)");
        // Should produce triples of distinct elements.
        bool found = any_output_starts_with(collector, "( triple a b c )") ||
                     any_output_starts_with(collector, "( triple a c b )") ||
                     any_output_starts_with(collector, "( triple b a c )") ||
                     any_output_starts_with(collector, "( triple b c a )") ||
                     any_output_starts_with(collector, "( triple c a b )") ||
                     any_output_starts_with(collector, "( triple c b a )");
        CHECK(found);
        // Must NOT produce any triple with repeated elements.
        CHECK_FALSE(any_output_contains(collector, "( triple a a"));
        CHECK_FALSE(any_output_contains(collector, "( triple b b"));
        CHECK_FALSE(any_output_contains(collector, "( triple c c")); });
}

TEST_CASE("inequality: functional property conflict detection (Wikidata pattern)")
{
    // Wikidata use case: a property is declared functional (single-valued),
    // but an item has two different values => contradiction.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(P is functional, A P X, A P Y, X != Y) => !
date_of_birth is functional
alice date_of_birth 1990
alice date_of_birth 1991
)");
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: functional property with same value is not a conflict")
{
    // Same property value entered twice (redundant, not contradictory).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(P is functional, A P X, A P Y, X != Y) => !
date_of_birth is functional
alice date_of_birth 1990
alice date_of_birth 1990
)");
        CHECK_FALSE(has_contradiction(collector)); });
}

// ---------------------------------------------------------------------------
// Meta-rules: predicates as first-class nodes
// ---------------------------------------------------------------------------

TEST_CASE("meta-rule: symmetric relation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is symmetric, X R Y) => (Y R X)
friend is symmetric
alice friend bob
)");
        CHECK(any_output_starts_with(collector, "( bob friend alice )")); });
}

TEST_CASE("meta-rule: opposite relation generates inverse")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R "is opposite of" S, X R Y) => (Y S X)
"has part" "is opposite of" "is part of"
chimpanzee "has part" hand
)");
        CHECK(any_output_starts_with(collector, "( hand \"is part of\" chimpanzee )")); });
}

// ---------------------------------------------------------------------------
// Multiple objects: unordered object set
// ---------------------------------------------------------------------------

TEST_CASE("multiple objects: unordered set with rule matching")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
alice parent_of bob charlie
(A parent_of B) => (B child_of A)
)");
        CHECK(any_output_starts_with(collector, "( bob child_of alice )"));
        CHECK(any_output_starts_with(collector, "( charlie child_of alice )")); });
}

// ---------------------------------------------------------------------------
// Deep unification: function composition (using lists for ordering)
// ---------------------------------------------------------------------------

TEST_CASE("deep unification: function composition as graph transformation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(F maps <A B>, G maps <B C>) => ((G compose F) maps <A C>)
f maps <item1 item2>
g maps <item2 item3>
)");
        CHECK(any_output_starts_with(collector, "((g compose f) maps <item1 item3>)")); });
}

// ---------------------------------------------------------------------------
// Constraint checking: graph coloring
// ---------------------------------------------------------------------------

TEST_CASE("constraint checking: valid graph coloring produces no contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A adjacent B, A color X, B color X) => !
r1 adjacent r2
r2 adjacent r3
r1 color red
r2 color blue
r3 color red
)");
        CHECK_FALSE(has_contradiction(collector)); });
}

TEST_CASE("constraint checking: invalid graph coloring produces contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A adjacent B, A color X, B color X) => !
r1 adjacent r2
r2 adjacent r3
r1 color red
r2 color red
)");
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("janet: zelph/sources returns only subjects, never objects")
{
    // Regression test: given a chain a R b R c, the sources of b must be
    // exactly {a}. The node c (where b is the *subject*) must not appear.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
nodeA P279 nodeB
nodeC P279 nodeB
nodeB P279 nodeD
)");
        collector.clear();
        interactive.process(R"(%(string/join (sorted (map (fn [n] (zelph/name n)) (zelph/sources "P279" (zelph/resolve "nodeB")))) ","))");
        CHECK(any_output_contains(collector, "nodeA,nodeC"));
        CHECK_FALSE(any_output_contains(collector, "nodeD")); });
}

TEST_CASE("janet: a statement ABOUT a fact is not a subject of that fact's predicate")
{
    // zelph/sources is documented to return "exactly those nodes S for which
    // the fact S predicate target exists". A fact that has the fact as its
    // SUBJECT -- a reified statement, a rule condition, the rule-pattern
    // marking -- is linked to it in both directions exactly like its own
    // subject, and the bidirectionality test could not tell the two apart. So
    // `(a p b) note ok` reported itself as a subject of `p`, and the SPARQL
    // layer, which is built on this primitive, returned it as a result row.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x p b
(a p b) note ok
)");
        collector.clear();
        interactive.process(R"(%(string "SRC-" (length (zelph/sources "p" "b"))))");
        CHECK(any_output_contains(collector, "SRC-2"));

        // The query is the second reading of the same question and answers
        // two; the count above must not exceed it.
        collector.clear();
        interactive.process("S p b");
        CHECK(collect_answers(collector).size() == 2);

        // The reifying statement is still reachable as what it is.
        collector.clear();
        interactive.process(R"(%(string "NOTE-" (length (zelph/sources "note" "ok"))))");
        CHECK(any_output_contains(collector, "NOTE-1")); });
}

TEST_CASE("janet: a self-fact has a subject and an object like any other")
{
    // `a p a` stores its object in the subject's bidirectional entry, so the
    // "object is unidirectional" test found no object at all and the "subject
    // is not the object" test dropped the subject: both traversals reported
    // NOTHING for a fact the query answers. Sets and lists are built on these
    // two functions, so this was not a curiosity of self-referential data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("m p m");

        collector.clear();
        interactive.process(R"(%(string "S-" (zelph/name (first (zelph/sources "p" "m")))))");
        CHECK(any_output_contains(collector, "S-m"));

        collector.clear();
        interactive.process(R"(%(string "T-" (zelph/name (first (zelph/targets "m" "p")))))");
        CHECK(any_output_contains(collector, "T-m"));

        collector.clear();
        interactive.process("S p m");
        CHECK(collect_answers(collector).size() == 1); });
}

// ---------------------------------------------------------------------------
// Variable name sharing across rules
// ---------------------------------------------------------------------------

TEST_CASE("naming: variable names re-used by a later rule keep earlier rules intact")
{
    // Each statement creates fresh variable nodes; rule topology is anchored
    // via core.Causes / core.Conjunction and stays unambiguous even when two
    // rules use the same variable NAMES (A, B, C). This test guards both the
    // functional property and the display: assigning "A" to rule 2's fresh
    // variable must not strip the name from rule 1's variable.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A ancestor B, B ancestor C) => (A ancestor C)
(R is transitive, A R B, B R C) => (A R C)
x ancestor y
y ancestor z
5 > 4
4 > 3
> is transitive
)");
        // Functional: BOTH rules fire despite shared variable names.
        CHECK(any_output_starts_with(collector, "( x ancestor z )"));
        CHECK(any_output_starts_with(collector, "( 5 > 3 )"));

        // Display: rule 1's variables are still shown by name in
        // .list-rules -- which prints the same unmarked form as every other
        // command, so a listed rule can be pasted straight back in.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(A ancestor B)"));
        CHECK_FALSE(any_output_contains(collector, "?? ancestor")); });
}

// ---------------------------------------------------------------------------
// Bound-pattern grounding: exact object-set semantics for nested patterns
// ---------------------------------------------------------------------------

TEST_CASE("grounding: fully bound nested pattern requires the exact fact node")
{
    // Documents a deliberate design decision of bound-pattern grounding
    // (ground_pattern in unification.cpp): once all variables of a
    // structured condition SUBJECT are bound, the pattern denotes exactly
    // one fact node, resolved via hash lookup with EXACT object-set
    // semantics -- consistent with instantiate_fact() and the termination
    // guard. Deep unification's greedy subset matching of objects is
    // deliberately NOT replicated at this point: a graph fact carrying
    // additional objects is a different node and is not found. (Top-level
    // condition objects keep the documented "objects as alternatives"
    // subset semantics via extract_bindings.) To restore subset matching
    // for this corner case, ground_pattern would have to return Unbound
    // instead of Missing -- at the cost of falling back to a full scan.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x trigger b1
(a d+ b1 b2) ci c
((a d+ b1 b2) ci c) co e
(x trigger B, ((a d+ B) ci c) co E) => (found B E)
)");
        // The first condition binds B = b1 (bound subject x guarantees it
        // is evaluated first). The grounded pattern ((a d+ b1) ci c) then
        // denotes a fact node that does not exist -- only the multi-object
        // variant ((a d+ b1 b2) ci c) does -- so the condition fails
        // instead of subset-matching the multi-object fact.
        CHECK_FALSE(any_output_starts_with(collector, "( found")); });
}

TEST_CASE("grounding: fully bound nested pattern anchors on the exact fact node")
{
    // Positive control for the exact-object semantics above: with the
    // exact single-object fact present, grounding resolves the pattern to
    // the concrete node and the rule fires via a direct anchor (no scan).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
x trigger b1
(a d+ b1) ci c
((a d+ b1) ci c) co e
(x trigger B, ((a d+ B) ci c) co E) => (found B E)
)");
        CHECK(any_output_starts_with(collector, "( found b1 e )")); });
}

// ---------------------------------------------------------------------------
// Template rejection: variables at depth >= 2
// ---------------------------------------------------------------------------

TEST_CASE("unification: consequence templates with variables only at depth 2 are not matched as data")
{
    // Distilled from the multiplication junk-fact regression: the first
    // rule's consequence pattern exists in the graph as an out-fact whose
    // ONLY variable A sits at structural depth 2 -- its subject decomposes
    // to a hash node plus the constant 0, so a shallow template check sees
    // nothing. The second rule scans out-facts; before the deep template
    // reject it matched the template, bound X to a variable-containing
    // node, and deduced junk.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((A probe nil) state 0) => (((A probe nil) state 0) out nil)
(X out P) => (X leaked P)
)");
        // IMPORTANT: clear AFTER defining the rules. The REPL echoes rule
        // definitions, and the echo of rule 2 literally contains the word
        // "leaked" -- the original version of this test checked the
        // collector including that echo and failed even though the engine
        // behaved correctly.
        collector.clear();
        interactive.process("seed1 unrelated seed2");
        interactive.run(true, false, false);
        CHECK_FALSE(any_output_contains(collector, "leaked"));

        // Positive control: a concrete state fact drives the same pipeline
        // legitimately -- rule 1 derives a ground out-fact, rule 2 consumes it.
        collector.clear();
        interactive.process("(d1 probe nil) state 0");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "leaked")); });
}

// ---------------------------------------------------------------------------
// Self-referential facts: subject == object reconstruction
// ---------------------------------------------------------------------------

TEST_CASE("parse_fact: self-referential fact keeps its object once it becomes the subject of further facts")
{
    // fact() draws no separate object edge when object == subject; the
    // subject IS the object, and parse_fact repairs the reconstructed
    // object set accordingly. The repair existed only in the
    // single-candidate branch: as soon as the self-referential fact became
    // the SUBJECT of other facts (their backlinks add bidirectional
    // neighbors), the disambiguation path dropped the implicit object --
    // rendering ((x foo ?) ...) and handing an empty object set to every
    // parse_fact consumer (node_to_string, deduce, != guard, negation).
    // Division X/X surfaced this systematically.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x foo x
(x foo x) bar a
)");
        // This third statement's echo renders the inner (x foo x) while a
        // second consumer (the bar fact) already exists -- the exact
        // constellation that forced the disambiguation path.
        collector.clear();
        interactive.process("(x foo x) baz b");
        CHECK(any_output_contains(collector, "x foo x"));
        CHECK_FALSE(any_output_contains(collector, "foo ?")); });
}

TEST_CASE("rules: a chained => says which arrow has to be parenthesised")
{
    // "A => B => C" is one statement whose predicate `=>` also stands among
    // its objects, so it lands in the generic "same relation type and
    // object" refusal -- accurate, and no help at all to someone writing a
    // rule whose conclusion is a rule. Which arrow binds tighter is
    // genuinely undecided, so the answer is a demand to say, not a default.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("(R is transitive) => (X R Y, Y R Z) => (X R Z)"),
                             doctest::Contains("has to be parenthesised"),
                             std::runtime_error);

        // The parenthesised form is accepted.
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))"); });
}

TEST_CASE("rules: a ground condition is not a non-match")
{
    // Matching a condition that contains no variable binds nothing, and the
    // engine read "nothing bound" as "no match" -- which silently disabled
    // every rule one of whose conditions happens to name its nodes. Both
    // conditions here are satisfied, so the rule has to fire.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(a p b, X q d) => (X r s)");
        interactive.process("a p b");
        collector.clear();
        interactive.process("e q d");
        CHECK(any_deduction_of(collector, "e r s"));

        collector.clear();
        interactive.process("X r Y");
        CHECK(answers_contain(collector, "e r s")); });
}

TEST_CASE("rules: a ground condition that does NOT hold blocks the rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b, X q d) => (X r s)");
        interactive.process("e q d");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X r Y");
        CHECK_FALSE(answers_contain(collector, "e r s")); });
}

TEST_CASE("rules: a negated consequence is refused, not ignored")
{
    // "(A p B) => ¬(A q B)" used to derive (x q y) from (x p y) -- the exact
    // opposite of what it says, in silence: deduce reads the consequence's
    // predicate and creates the fact, and the negation tag sits beside the
    // pattern where nothing on that path looks.
    //
    // The test has to be asked of the SYNTAX. The tag is a fact ABOUT the
    // pattern node, and a ground pattern is hash-consed, so a pattern negated
    // in one rule carries the tag in every other rule that mentions it --
    // only the statement itself knows where the "¬" was written.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const char* rule : {"(A p B) => ¬(A q B)",
                                 "(A p B) => (¬(A q B))",
                                 "(A p B) => (A q B) ¬(A r B)"})
        {
            CHECK_THROWS_WITH_AS(interactive.process(rule),
                                 doctest::Contains("condition operator"),
                                 std::runtime_error);
        }

        // A negated CONDITION is untouched, including when its pattern is
        // ground and therefore shared with whatever else mentions it.
        interactive.process("(A p B, ¬(A r B)) => (A q B)");
        collector.clear();
        interactive.process("x p y");
        CHECK(any_deduction_of(collector, "x q y"));

        collector.clear();
        interactive.process("X q Y");
        CHECK(answers_contain(collector, "x q y")); });
}

TEST_CASE("rules: a nested negation is refused, not silently halved")
{
    // "¬" tags the pattern node, and tagging it twice is tagging it once, so
    // "¬(¬(F))" meant "¬(F)" -- the exact opposite of a double negation. The
    // rule then fired precisely when it should not have, in silence. Reading
    // it properly needs a second negation stratum, which is the parked
    // feature "¬(A, B)" is refused for; saying so beats dropping half the
    // statement.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const char* rule : {"¬(¬(a p b)) => (c q d)",
                                 "¬((¬(a p b))) => (c q d)",
                                 "(X p Y, ¬(¬(X q Y))) => (X r Y)"})
        {
            CHECK_THROWS_WITH_AS(interactive.process(rule),
                                 doctest::Contains("does not nest"),
                                 std::runtime_error);
        }

        // One level is untouched, and still means what it says: the rule
        // fires while the fact is absent and not once it is there.
        interactive.process("¬(a p b) => (c q d)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));

        interactive.process("m p n");
        interactive.process("¬(m p n) => (e r f)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("E r F");
        CHECK_FALSE(answers_contain(collector, "e r f")); });
}

TEST_CASE("rules: two consequences are two objects, not a conjunction")
{
    // "A => (B, C)" builds a rule whose consequence is a conjunction SET.
    // The engine deduces the OBJECTS of a `=>` fact and has no reading for
    // a set node in that position, so the rule was accepted, listed by
    // .list-rules -- and derived nothing at all, in silence. zelph can say
    // what was meant; it is spelled with several objects.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH_AS(interactive.process("(X p Y) => (X q N, N r Y)"),
                             doctest::Contains("several objects"),
                             std::runtime_error);

        // The form the message names works, and the two consequences share
        // the fresh variable N -- which is the reason it is one rule and
        // not two.
        interactive.process(".deductions all");
        interactive.process("(X p Y) => (X q N) (N r Y)");
        collector.clear();
        interactive.process("a p b");
        REQUIRE(any_deduction_of(collector, "a q"));
        CHECK(any_deduction_of(collector, "r b"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "=>")); });
}

// ---------------------------------------------------------------------------
// Fresh variables: a firing creates a witness solely when no prior
// instance is present
// ---------------------------------------------------------------------------
//
// A variable that only a consequence names is considered fresh: firing the
// rule creates a node for it, provided no existing nodes already make every
// consequence a fact (logic.md, "No duplicate witnesses"). The check was
// implemented as a manually coded walk over the graph, and each scenario
// listed below reflects a shape it got wrong. In three instances, the old
// walk allowed the rule to produce a witness on every pass, causing check
// mode -- repeating classic passes until no further changes occur -- to
// never terminate. These tests are executed with `.semi-naive on`, ensuring
// the outdated implementation fails them rather than entering an infinite
// loop.

TEST_CASE("fresh variables: a witness the search meets later is still found")
{
    // `a q mk` and `mk r b` jointly serve as a witness for (X q N) (N r Y)
    // at a p b. The walk bound N to the first node it encountered via a
    // single consequence and did not attempt any alternative. In the first
    // shape shown, six nodes reside within `r b` while only one of them
    // lies within `a q`, and within this graph the walk overlooked the
    // witness for every k and generated new ones. The second shape, being
    // the mirror image, never encountered failure here. It stays because
    // the walk commenced with whichever consequence the rule's set listed
    // first, and that order is not subject to the rule's discretion.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => (X q N) (N r Y)");

        for (const int shape : {1, 2})
            for (int k = 1; k <= 6; ++k)
            {
                const std::string id = std::to_string(shape) + std::to_string(k);
                for (int i = 1; i <= 6; ++i)
                {
                    const std::string m = "m" + id + "_" + std::to_string(i);
                    interactive.process(shape == 1 ? m + " r b" + id : "a" + id + " q " + m);
                }
                const std::string mk = "m" + id + "_" + std::to_string(k);
                interactive.process(shape == 1 ? "a" + id + " q " + mk : mk + " r b" + id);
            }

        for (const int shape : {1, 2})
            for (int k = 1; k <= 6; ++k)
            {
                const std::string id = std::to_string(shape) + std::to_string(k);
                interactive.process("a" + id + " p b" + id);
            }
        interactive.process("u v w");

        for (const char* query : {"X q Y", "X r Y"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            for (const std::string& answer : collect_answers(collector))
                CHECK(answer.find("??") == std::string::npos);
        } });
}

TEST_CASE("fresh variables: a self-fact is a witness")
{
    // `a q a` makes (A q B) a fact when A equals a and B equals a. The
    // walk sought an object distinct from the subject, thus never counted
    // a self-fact and created `a q ??` beside it -- and from the object
    // side `?? t c` beside `c t c`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is p) => (A q B)");
        interactive.process("(A is s) => (B t A)");
        interactive.process("a q a");
        interactive.process("c t c");
        interactive.process("a is p");
        interactive.process("c is s");
        interactive.process("u v w");

        for (const char* query : {"A q B", "A t B"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            const auto answers = collect_answers(collector);
            CHECK(answers.size() == 1);
            for (const std::string& answer : answers)
                CHECK(answer.find("??") == std::string::npos);
        } });
}

TEST_CASE("fresh variables: a head whose variables are all fresh holds through any fact")
{
    // In (A is human) => (N knows M), the consequence remains unconnected
    // to A, meaning every `knows` fact serves as a witness, and after one
    // such fact comes into existence, no human adds another. The walk gave
    // up upon both subject and object being fresh, thus each firing
    // created a pair, and each subsequent input line fired the rule anew
    // for every human: five pairs in total by the end of this test.
    //
    // `.semi-naive on`: refer to the note located
    // directly above this group.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");

        interactive.process("(A is human) => (N knows M)");
        interactive.process("k knows l");
        interactive.process("tim is human");
        interactive.process("bob is human");
        interactive.process("u v w");

        collector.clear();
        interactive.process("N knows M");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k knows l"});

        // Without such a fact, the first firing generates a single pair,
        // and this pair serves as the witness for the subsequent one.
        interactive.process("(A is cat) => (N likes M)");
        interactive.process("felix is cat");
        interactive.process("tom is cat");
        interactive.process("u v w");

        collector.clear();
        interactive.process("N likes M");
        CHECK(collect_answers(collector) == std::vector<std::string>{"?? likes ??"}); });
}

TEST_CASE("fresh variables: a rule's own consequence pattern is no witness")
{
    // The consequence of a rule is a pattern within the graph, and the
    // walk took it for a fact: `k likes N` -- this rule's own -- satisfied
    // k likes ??, and `a q W` from a different rule satisfied a q ??.
    // Neither rule was ever fired. A pattern carries variables and holds
    // nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is human) => (k likes N)");
        interactive.process("(Z r t) => (a q W)");
        interactive.process("(A is p) => (A q B)");
        interactive.process("s is human");
        interactive.process("a is p");

        collector.clear();
        interactive.process("k likes N");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k likes ??"});

        collector.clear();
        interactive.process("a q B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a q ??"}); });
}

TEST_CASE("fresh variables: a consequence whose instance is its own pattern with a variable counts as there for every binding")
{
    // The sole variables within `k about ...` sit in the conditions of the
    // rule it mentions, so a firing writes it precisely as written: its
    // instance is the rule's own consequence pattern containing variables,
    // which constitutes rule text, and a firing does not derive it. It is
    // counted as present, and the check for an earlier witness asks for N
    // alone, which `z near g` and `y near h` supply. An engine whose first
    // firing claims that node counts it as missing for the first binding
    // only: `g is m` makes a second witness beside `z near g`, and
    // `h is m`, firing afterwards, creates none.
    //
    // Single passes, as counts_over_passes runs them: an engine that counts
    // the pattern as missing for each binding makes a new witness on every
    // pass, and a run to the fixpoint would not end.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive off");
        interactive.process(".auto-run"); // a toggle: off
        process_lines(interactive, R"(
(A is m) => (k about ((C r D, C s D) => (c q d))) (N near A)
z near g
y near h
g is m
.run-once
h is m
.run-once
.run-once
)");

        collector.clear();
        interactive.process("X near g");
        CHECK(collect_answers(collector) == std::vector<std::string>{"z near g"});

        collector.clear();
        interactive.process("X near h");
        CHECK(collect_answers(collector) == std::vector<std::string>{"y near h"}); });
}

TEST_CASE("fresh variables: a witness whose fact mentions a rule with variables in its conditions is found")
{
    // A firing keeps the condition set of a rule its consequence refers to,
    // so the first rule writes `w about R` with A and B still present in R's
    // conditions, while the second rule, acting as a generator, produces
    // `R' noted w` with X, G, and Y still part of the conditions of R'. For
    // a query, such a fact is regarded as rule text, not data
    // (Zelph::var_in_closure reads a rule's conditions), and the check for
    // an earlier witness read its candidates in the same way: it never found
    // the fact the firing had generated, either via its object or its
    // subject, and each pass created a new witness. Under `.semi-naive off`,
    // the run never ended. In the one-condition twins, A and B, or X and Y,
    // are fresh and the fact is ground, meaning it was found all along.
    //
    // `.semi-naive on`, and the first passes behind REQUIRE: an engine with
    // the defect fails at this stage rather than causing a hang during the
    // classic run that follows.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");
        interactive.process(".auto-run"); // off: the rules fire upon the first .run
        process_lines(interactive, R"(
(X p Y) => (W about ((A r B, A s B) => (c q d)))
(G go H) => (((X G Y, X r Y) => (c q H)) noted W)
a p b
t go k
)");

        const std::size_t before = node_count(collector, interactive);
        interactive.process(".run");
        const std::size_t after = node_count(collector, interactive);
        // Two witnesses and the facts that name them; the second rule
        // additionally writes the rule it refers to, along with its
        // consequence `c q k`.
        CHECK(after == before + 6);

        for (const char* pass : {".run", ".run-once"})
        {
            INFO(pass);
            interactive.process(pass);
            REQUIRE(node_count(collector, interactive) == after);
        }

        interactive.process(".semi-naive off");
        interactive.process(".run");
        CHECK(node_count(collector, interactive) == after);

        // Once the variable store is switched off, the walk that
        // answers does so by reading the candidates in the same way.
        interactive.process(".fact-stores off");
        interactive.process(".run-once");
        CHECK(node_count(collector, interactive) == after); });
}

TEST_CASE("fresh variables: a fresh variable nested in a consequence is created once")
{
    // ((A f N) q b) holds as long as there is at least one instance of
    // `(a f n) q b`. The walk examined solely the outermost layer of a
    // consequence and never located N within its subject, thus each
    // input line generated a new (a f ??) q b.
    //
    // `.semi-naive on`: refer to the note located
    // directly above this group.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");

        interactive.process("(A is p) => ((A f N) q b)");
        interactive.process("a is p");
        interactive.process("c d e");

        collector.clear();
        interactive.process("X q b");
        // "?\?" instead of "??": "??)" would constitute a
        // trigraph.
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a f ?\?) q b"}); });
}

TEST_CASE("fresh variables: the consequence that can bind a witness is joined first")
{
    // The check joins the consequences sequentially, and Unification
    // compares a container through its node, thus binding no variable within
    // one. It took the consequences in the order of their node ids, and a
    // fresh W located in a collection of one consequence and outside in
    // another was identified solely when the latter appeared first;
    // otherwise, each firing made a new witness. The precedence of which one
    // comes first depends on the ids of the rules' collections, making the
    // order unreliable to rely on.
    //
    // `.semi-naive off` and single passes: an engine afflicted by the
    // defect accumulates a witness with each pass, rather than never
    // ending the run.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string rule;
        SUBCASE("a membership binds it") { rule = "(X p Y) => (X likes @{(W s Y)}) (W in @{k})"; }
        SUBCASE("an ordinary fact binds it") { rule = "(X p Y) => (X likes @{(W s Y)}) (X r W)"; }
        SUBCASE("two collections hold it") { rule = "(X p Y) => (X likes @{(W s Y)} @{(W t Y)}) (W r X)"; }
        SUBCASE("it is a member itself") { rule = "(X p Y) => (W in @{k}) (X likes @{W})"; }

        interactive.process(".semi-naive off");
        interactive.process(".auto-run"); // off: each pass is run below
        interactive.process(rule);
        interactive.process("a p b");
        interactive.process(".run-once");

        const std::size_t after = node_count(collector, interactive);
        for (int pass = 0; pass < 3; ++pass)
        {
            interactive.process(".run-once");
            CHECK(node_count(collector, interactive) == after);
        } });
}

TEST_CASE("fresh variables: a fresh predicate is matched like any other position")
{
    // The walk gave up immediately upon encountering a consequence
    // predicate that was fresh, plain or nested within a composite
    // predicate, causing such a rule to fire anew with each input line.
    //
    // `.semi-naive on`: refer to the note located
    // directly above this group.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive on");

        interactive.process("(A is p) => (A N b)");
        interactive.process("(A is p) => (A (N r s) c)");
        interactive.process("a is p");
        interactive.process("u v w");

        for (const char* query : {"a R b", "a R c"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).size() == 1);
        } });
}

TEST_CASE("fresh variables: each binding of the conditions gets its own witness")
{
    // On the opposite side of the check: the witness for tim provides no
    // information regarding bob, hence each gets one, and no firing is
    // duplicated. logic.md shows this rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is human) => (B nameof A)");
        interactive.process("tim is human");
        interactive.process("bob is human");
        interactive.process("u v w");

        collector.clear();
        interactive.process("X nameof Y");
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        CHECK(answers == std::vector<std::string>{"?? nameof bob", "?? nameof tim"}); });
}

TEST_CASE("fresh variables: the fact that triggers the rule can be its witness")
{
    // (X p Y) => (X p Z) at a p b holds when Z equals b, hence the rule
    // creates nothing. If the fact were not a witness, each newly
    // created node would trigger the rule once more, causing it to run
    // indefinitely.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => (X p Z)");
        interactive.process("a p b");
        interactive.process("u v w");

        collector.clear();
        interactive.process("X p Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"}); });
}

TEST_CASE("fresh variables: a witness inside a rule's own collection is found again")
{
    // A fresh variable that also stands in a collection of the rule's text
    // uniquely identifies the term that the firing constructs there, meaning
    // the fact containing the term can only be predicted once the witness is
    // bound: the check binds it from the consequence that contains it
    // externally to the collection first, and a collection matches each term
    // a firing builds until the join confirms the specific term this firing
    // produces. Predicted before, the term referred to a collection lacking
    // the witness, which does not exist, and the rule made a new witness on
    // each pass.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("beside it in the same consequence")
        {
            const auto counts = counts_over_passes(collector, interactive, "(X p Y) => (X likes @{(W s Y)} W)\nb p k");
            CHECK(counts == std::vector<std::size_t>(4, counts.front()));

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).size() == 2);
        }
        SUBCASE("in two consequences")
        {
            const auto counts = counts_over_passes(collector, interactive, "(X p Y) => (X likes @{(W s Y)}) (W r @{(W s Y)})\na p b");
            CHECK(counts == std::vector<std::size_t>(4, counts.front()));
        }
        SUBCASE("written into")
        {
            for (const std::string rule : {"(X p Y) => (W likes @{W})", "(X p Y) => (W likes @{X W})"})
            {
                CAPTURE(rule);
                interactive.process(".new");
                const auto counts = counts_over_passes(collector, interactive, rule + "\na p b\nc p d");
                CHECK(counts == std::vector<std::size_t>(4, counts.front()));
            }
        } });
}

TEST_CASE("fresh variables: shapes with a witness and a rule's own collection end")
{
    // Each shape puts a fresh variable into a collection within the rule's
    // text at a different location: inside, outside, both, nested, written
    // into. Each ends, with a node count that the third pass no longer
    // changes.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const std::string shape : {"(X p Y) => (X likes @{W}) (W r Y)\na p b",
                                        "(X p Y) => (X likes {W}) (W r Y)\na p b",
                                        "(X p Y) => (X likes @{W Y}) (W r Y)\na p k\nb p k",
                                        "(X p Y) => (X likes {W Y}) (W r Y)\na p k\nb p k",
                                        "(X p Y) => (X likes @{(W s @{Y})}) (W r X)\na p b",
                                        "(X p Y) => (W in @{X}) (W r Y)\na p b\nc p d",
                                        "(X p Y) => ((W in @{Y}) is noted) (W r X)\na p b\nc p b",
                                        "(X p Y) => (X likes @{@{W}}) (W r X)\na p b",
                                        "(X p Y) => (X likes @{(W s Y)} @{(W t Y)}) (W r X)\na p b",
                                        "(G go H) => ((X G Y) => (X likes @{(W s H)}) (W r X))\nt go k\na t b",
                                        "(X p Y) => (W in @{W Y})\na p b\nc p b",
                                        "(X p Y) => (X likes {(W s Y)}) (W r X)\na p b",
                                        "(X p Y) => (W in @{k}) (X likes @{W})\na p b",
                                        "(X p Y) => (W in @{(W s Y)})\na p b",
                                        "(G go H) => ((X G Y) => (W in @{H}) (X r W))\nt go k\na t b\nc t d",
                                        "(G go H) => (((X G Y) => (Y q @{(W s H)})) noted W)\nt go k\nu go m",
                                        "(X p Y) => (X likes @{(W s Y)} W) (W in @{k})\na p b",
                                        "(X p Y) => (X likes @{(W s Y)}) (W r k)\na p b\nk r k",
                                        "(X p Y) => (((W r X, W s Y) => (c q d)) is noted) (W t X)\na p b",
                                        "(X p Y) => (X likes @{((W r X, W s Y) => (c q d))}) (W t X)\na p b",
                                        "(X p Y) => (W in @{((A r X, A s Y) => (c q d))}) (W t X)\na p b"})
        {
            CAPTURE(shape);
            interactive.process(".new");
            // A shape that generates its rule fires it solely during the
            // second pass, thus allowing the count to stabilize from that
            // point onward.
            const auto counts = counts_over_passes(collector, interactive, shape);
            CHECK(counts[2] == counts[1]);
            CHECK(counts[3] == counts[1]);
        } });
}

TEST_CASE("rules: a condition that is not a pattern is refused, not carried")
{
    // "(*A p C, C q b) => (A marked yes)" built a rule whose first condition
    // was the NODE A. The focus operator did exactly what it promises -- the
    // expression evaluates to the focused node rather than to the fact -- but
    // a node carries no statement, so unification had nothing to match and the
    // rule could never fire. It was accepted all the same, and printed with
    // the offending member left out (an unbound variable is not shown as a
    // container element), so .list-rules showed a one-condition rule that
    // looked as if it worked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const char* rule : {"(*A p C, C q b) => (A marked yes)",
                                 "(C q b, *A p C) => (A marked yes)",
                                 "(a, (c q b)) => (x marked yes)",
                                 // The documented explicit spelling of the
                                 // comma list has to agree with it.
                                 "(*{a (c q b)} ~ conjunction) => (x marked yes)"})
        {
            CHECK_THROWS_WITH_AS(interactive.process(rule),
                                 doctest::Contains("can never match"),
                                 std::runtime_error);
        }

        // The form the message names is the one that was meant.
        interactive.process("(A p C, C q b) => (A marked yes)");
        collector.clear();
        interactive.process("u p c");
        interactive.process("c q b");
        CHECK(any_deduction_of(collector, "u marked yes"));

        // A focus one level DOWN stays legitimate: the member evaluates to
        // the fact "A q b", with "A p c" built on the side. What is asked is
        // what a condition evaluates TO, not how it is written.
        interactive.process(".new");
        interactive.process("((*A p c) q b, A r d) => (A marked yes)");
        collector.clear();
        interactive.process("v q b");
        interactive.process("v r d");
        CHECK(any_deduction_of(collector, "v marked yes")); });
}

TEST_CASE("rules: zelph/rule refuses a condition that is not a pattern")
{
    // The Janet API is the other door to the same inert rule, and a generator
    // that builds one has no way of noticing: the rule enters the graph, is
    // listed, and derives nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process(R"js(%(zelph/rule [(zelph/resolve "a")] (zelph/fact "x" "q" "y")))js"),
                             doctest::Contains("can never match"),
                             std::runtime_error); });
}

TEST_CASE("naming: a query variable does not take a real node's name")
{
    // Variable names are cosmetic and statement-scoped, but they went into
    // the same map as real names and won. A graph holding a node named "A"
    // -- a single-letter Wikidata label is enough -- lost that name to the
    // first query that mentioned the variable A, and the node afterwards
    // rendered as "(?? ?? ??)". Asking a question deleted data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("alpha rel beta");
        interactive.process(".name alpha A");

        collector.clear();
        interactive.process("A rel beta");
        // The answer binds the variable A to the node NAMED "A", and that
        // name is quoted on the way out: bare, the line would read back as
        // the very query that produced it rather than as its answer.
        CHECK(answers_contain(collector, "\"A\" rel beta"));
        CHECK_FALSE(any_output_contains(collector, "??"));

        // The name still resolves to the node it was given to.
        collector.clear();
        interactive.process(".node A");
        CHECK(any_output_contains(collector, "Name in language"));
        CHECK_FALSE(any_output_contains(collector, "No node found")); });
}

TEST_CASE("rules: a rule whose only condition quantifies over predicates fires")
{
    // "For every declared relation type R, ..." is the shape that makes zelph
    // different from a query engine, and as the SOLE condition of a rule it
    // derived nothing at all -- silently. Adding any second condition, even a
    // pure guard like `R != p`, made it work again, which is why it went
    // unnoticed: every example that quantifies over predicates in the stdlib
    // and the documentation carries a second condition.
    //
    // The cause was in Zelph::filter, the three-argument form that answers
    // "which node of this fact is its predicate". A fact's outgoing edges
    // hold its PARENTS as well as its subject and predicate, so the
    // consequence `R declared yes` has the rule among them -- and the rule
    // points at its own subject, the condition `R ~ ->`. That condition has
    // the right predicate and the right object, so the walk reported the RULE
    // as a second relation type of the consequence, and deduce() refused the
    // ambiguity. The exact probe `check_fact(nd, ~, ->)` asks the question
    // that was meant: not "does nd reach such a fact" but "is nd its
    // subject".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");

        collector.clear();
        interactive.process("(R ~ ->) => (R declared yes)");
        interactive.process(".run");

        collector.clear();
        interactive.process("S declared yes");

        // Every relation type in the graph: the one the data declared, the
        // rule's own consequence predicate, and the core vocabulary --
        // including `~` itself, which is a relation type declared by a fact
        // whose subject IS its predicate.
        CHECK(answers_contain(collector, "p declared yes"));
        CHECK(answers_contain(collector, "declared declared yes"));
        CHECK(answers_contain(collector, "~ declared yes"));
        CHECK(answers_contain(collector, "cons declared yes"));
        CHECK(answers_contain(collector, "in declared yes"));

        // Not a relation type: `a` and `b` are data, `->` is the category.
        CHECK_FALSE(answers_contain(collector, "a declared yes"));
        CHECK_FALSE(answers_contain(collector, "b declared yes"));
        CHECK_FALSE(answers_contain(collector, "-> declared yes"));

        // A COMPOSITE predicate is bound like any other. logic.md claims
        // exactly this -- "the fact is found by a rule quantifying over
        // predicates just like any other" -- and the claim was false for the
        // single-condition form the sentence describes.
        interactive.process("x (a p b) y");
        interactive.process(".run");

        collector.clear();
        interactive.process("S declared yes");
        CHECK(answers_contain(collector, "(a p b) declared yes")); });
}

TEST_CASE("rules: a consequence subject that is itself a predicate stays the subject")
{
    // The control for the fix above: the exact probe replaced the
    // neighbourhood walk, but the SUBJECT exclusion in front of it still has
    // to hold. It only bites when the consequence's subject is a GROUND node
    // that is itself a declared relation type -- `p` here, used as data by a
    // rule that has nothing to do with predicates. Drop the exclusion and
    // `check_fact(p, ~, ->)` succeeds, the consequence has two candidate
    // predicates, and deduce() refuses it.
    //
    // A variable subject would not do: the pattern node carries the variable,
    // and a variable is not a declared relation type, so the test would pass
    // either way. That was the first version of this case, and it was
    // vacuous.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("alarm ranks high");

        collector.clear();
        interactive.process("(X ranks high) => (p scored X)");
        interactive.process(".run");

        collector.clear();
        interactive.process("p scored O");
        CHECK(answers_contain(collector, "p scored alarm")); });
}

TEST_CASE("rules: a composite predicate in a consequence is instantiated")
{
    // deduce() substituted a predicate that IS a variable and nothing else, so
    // a COMPOSITE one kept the rule's own variables. `(X p Y) => (X (Y r s) c)`
    // derived `a (Y r s) c` -- a fact carrying a template variable, which no
    // query can match and which the ground guard did not catch either, because
    // that guard read the subject and the objects but not the predicate.
    //
    // Both halves are pinned here: the predicate is substituted, and nothing
    // with a residual variable reaches the graph.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(q r s) ~ ->
(X p Y) => (X (Y r s) c)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S Q O");
        CHECK(answers_contain(collector, "a (b r s) c"));
        CHECK_FALSE(answers_contain(collector, "a (Y r s) c"));

        // The instantiated predicate is declared as a relation type, which is
        // what keeps the derived fact readable after a reload.
        collector.clear();
        interactive.process("S ~ ->");
        CHECK(answers_contain(collector, "(b r s) ~ ->")); });
}

TEST_CASE("rules: a container in predicate position is rebuilt like any other")
{
    // Same path, reached through a container rather than a fact: the
    // predicate is not exempt from the rebuild that objects get, since only
    // the object of a PartOf deduction is written INTO.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(X p Y) => (X {Y} c)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S Q O");
        CHECK(answers_contain(collector, "a {b} c"));
        CHECK_FALSE(answers_contain(collector, "a @{Y} c")); });
}

TEST_CASE("rules: a composite predicate in a condition unifies structurally")
{
    // Subject and object positions have unified structurally all along --
    // `((Y r s) p Z)` and `(X p (Y r s))` both match -- but the predicate was
    // compared by IDENTITY, so `(X (Y r s) Z)` matched nothing whatsoever: the
    // graph holds `(b r s)`, never `(Y r s)`. The rule was accepted and
    // silently inert, and no binding order helped, because the candidate set
    // is fixed when the condition is set up rather than when it is joined.
    //
    // The candidate set is now the one a predicate VARIABLE gets; what
    // separates the two is that extract_bindings unifies the pattern against
    // each candidate instead of binding one variable to it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a (b r s) c
d (e r s) f
a (b r t) c
a p c
(X (Y r s) Z) => (Y links X)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S links O");
        CHECK(answers_contain(collector, "b links a"));
        CHECK(answers_contain(collector, "e links d"));

        // The fixed parts of the pattern still select: `(b r t)` differs in
        // its object, `p` is not composite at all.
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("rules: a pattern predicate is narrowed by its own predicate")
{
    // A predicate pattern gets the candidate set a predicate VARIABLE gets --
    // every declared relation type -- which is correct but is the cost of a
    // variable, and the pattern says far more than a variable does. A
    // candidate has to unify with `(Y r s)`, and unify_nodes matches
    // predicates before anything else, so no fact whose predicate is not `r`
    // can survive; the candidates are therefore the facts of `r` alone.
    //
    // What is checked here is that the narrowing loses NOTHING. `bulk` is the
    // relation the narrowed scan never looks at, and its facts must be
    // exactly as absent from the answers as they were before.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a (b r s) c
d (e r s) f
g (h r t) i
j (k q s) l
m bulk n
o bulk p
(X (Y r s) Z) => (Y links X)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S links O");
        CHECK(answers_contain(collector, "b links a"));
        CHECK(answers_contain(collector, "e links d"));

        // `(h r t)` differs in the object, `(k q s)` in the predicate, and
        // `bulk` is not composite at all.
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("rules: a pattern predicate whose own predicate is a variable still matches")
{
    // The fallback the narrowing needs: with `(Y R s)` there is no ground
    // predicate to narrow by, so the candidate set stays every declared
    // relation type -- and the rule has to keep working, or the optimisation
    // would have silently taken a shape away.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a (b r s) c
d (e q s) f
g (h r t) i
(X (Y R s) Z) => (Y links Z)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S links O");
        CHECK(answers_contain(collector, "b links c"));
        CHECK(answers_contain(collector, "e links f"));

        // `(h r t)` still differs in the object.
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("reasoning: the two strategies agree on what is REPORTED, not only on what is derived")
{
    // `.semi-naive check` compares derived FACTS. Anything the engine REPORTS
    // rather than derives -- a contradiction, and since this session a refusal
    // -- is invisible to it by construction, so a divergence there would be
    // caught by nothing at all.
    //
    // This closes that gap for one deliberately awkward network: a rule
    // GENERATOR writes the transitivity rule, the closure it produces is what
    // a contradiction rule then fires on (so the contradiction depends on
    // DERIVED facts, which is where delta seeding differs from a classic
    // pass), a COMPOSITE PREDICATE PATTERN runs beside it, and a REFUSED
    // deduction is reported from a third rule. Both strategies have to agree
    // on the derived facts AND on how many `!` lines come out.
    const std::string network = R"(
p is transitive
a p b
b p c
c p d
m (n r s) o
z rel {a b}
q p2 r
(R is transitive) => ((X R Y, Y R Z) => (X R Z))
(A p d, A p b) => !
(X (Y r s) Z) => (Y links X)
(X p2 Y) => (X in {a b})
)";

    const auto contradiction_lines = [](const zelph::io::OutputCollector& c)
    {
        return std::count_if(c.events().begin(), c.events().end(), [](const auto& e)
                             { return normalize(e.text).find("⇐") != std::string::npos
                                   && normalize(e.text).starts_with("!"); });
    };

    const auto run_with = [&](const char* mode)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(mode);
        process_lines(interactive, network);
        interactive.run(true, false, false);

        const auto bangs = contradiction_lines(collector);

        collector.clear();
        interactive.process("S p O");
        auto answers = collect_answers(collector);

        collector.clear();
        interactive.process("S links O");
        for (const auto& a : collect_answers(collector))
            answers.push_back(a);

        std::sort(answers.begin(), answers.end());
        return std::make_pair(answers, bangs);
    };

    const auto delta   = run_with(".semi-naive on");
    const auto classic = run_with(".semi-naive off");

    // The closure, the pattern-predicate consequence, and nothing else.
    REQUIRE(delta.first.size() == 7);
    CHECK(delta.first == classic.first);

    // One contradiction from the closure, two refusals from the third rule --
    // and the same number either way.
    CHECK(delta.second > 0);
    CHECK(delta.second == classic.second);
}

TEST_CASE("rules: an untagged one-member condition container fires (topology level)")
{
    // THE case, asked of the reasoner alone. The rule is built through the
    // Janet API in ONE expression, so no parser decides anything about
    // conjunctions: a container holding exactly one condition, with NO
    // `~ conjunction` tag anywhere, and the rule fact on top of it.
    //
    // The tag says HOW members combine. For one member no combination can
    // differ from any other, so it cannot change what the rule means -- but
    // every reader keyed on it, and this topology was built, counted, listed
    // and never fired.
    //
    // One expression on purpose: each `%(...)` line triggers a run of its
    // own, and a run between the condition and the rule finds a rule-less
    // graph, which muddies what is being tested.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(
            R"js(%(zelph/fact (zelph/collection (zelph/fact (quote X) "p" (quote Y))) "=>" (zelph/fact (quote X) "q" (quote Y))))js");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1")); });
}

TEST_CASE("rules: a bare fact as the rule's subject fires (topology level)")
{
    // The control one level down: no container at all. This has always
    // worked and must keep working -- it is what the parser builds for a
    // single condition.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(
            R"js(%(zelph/fact (zelph/fact (quote X) "p" (quote Y)) "=>" (zelph/fact (quote X) "q" (quote Y))))js");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b")); });
}

TEST_CASE("rules: a single condition fires in every SYNTAX too")
{
    // The layer above: whatever the parser chooses to build for each
    // spelling -- with or without a container, with or without the tag --
    // the rule has to run. zelph is a living ontology, and a rule written
    // without sugar is the same rule.
    const char* spellings[] = {
        "(X p Y) => (X q Y)",                   // one condition, no container
        "{(X p Y)} => (X q Y)",                 // written out in set notation
        "*{(X p Y)} => (X q Y)",                // ... with the focus operator
        "(*{(X p Y)} ~ conjunction) => (X q Y)" // ... and tagged explicitly
    };

    for (const char* rule : spellings)
    {
        CAPTURE(rule);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        interactive.process("a p b");
        interactive.process(rule);
        interactive.run(true, false, false);

        // It fires ...
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));

        // ... it is a rule to every command that reports on rules ...
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "No rules found"));

        // ... and the derivation is reconstructible through it.
        collector.clear();
        interactive.process(".explain (a q b)");
        CHECK(any_output_contains(collector, "a p b"));
    }
}

TEST_CASE("rules: a single GROUND condition in set notation fires too")
{
    // The same shape with no variable, which builds a set CONSTANT rather
    // than a collection -- a different node kind on the same question.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("{(a p b)} => (c q d)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "a p b")); });
}

TEST_CASE("rules: several untagged members are still not a conjunction")
{
    // The other side of the ruling: with MORE than one member the tag is the
    // only thing that says how they combine, and other combinations are
    // conceivable. An untagged container of several conditions therefore
    // stays what it was -- one opaque condition that matches nothing -- and
    // is not silently read as a conjunction.
    //
    // Separate networks on purpose: writing both spellings into ONE network
    // makes the second line MENTION the rule the first one built, which is a
    // corner of its own (see Zelph::is_mentioned).
    SUBCASE("untagged")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
a r b
*{(X p Y) (X r Y)} => (X q Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());
    }

    SUBCASE("tagged")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
a r b
(*{(X p Y) (X r Y)} ~ conjunction) => (X q Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));
    }
}

TEST_CASE("rules: correcting an inert rule is not discarded as a duplicate")
{
    // A container of several conditions WITHOUT the conjunction tag is not
    // read as a set of conditions and cannot fire; the tagged one does. Two
    // rules that behave differently must not be identified -- but the
    // canonical form rendered both containers as a set, so the parse-time
    // deduplication recognised the tagged rule as the untagged one already
    // present and rolled it back. Nothing was created, and the user could
    // not repair a rule that does not work by entering it correctly.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
a r b
{(X p Y) (X r Y)} => (X q Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        REQUIRE(collect_answers(collector).empty()); // inert, as it must be

        // The correction arrives, and it is a rule of its own.
        interactive.process("(*{(X p Y) (X r Y)} ~ conjunction) => (X q Y)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1"));

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b")); });
}

TEST_CASE("rules: an identical rule is still recognised as a duplicate")
{
    // The control the fix must not cost: entering the SAME rule twice, in
    // either spelling, still yields one rule.
    SUBCASE("the comma sugar")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(X p Y, X r Y) => (X q Y)
(A p B, A r B) => (A q B)
)");
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1"));
    }

    SUBCASE("the tagged verbose form")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(*{(X p Y) (X r Y)} ~ conjunction) => (X q Y)
(*{(A p B) (A r B)} ~ conjunction) => (A q B)
)");
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1"));
    }

    SUBCASE("an untagged container twice")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
{(X p Y) (X r Y)} => (X q Y)
{(A p B) (A r B)} => (A q B)
)");
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 0"));
    }
}

TEST_CASE("rules: entering the same untagged container rule twice builds it once")
{
    // The dedup still has to recognise a repetition of a rule it does NOT
    // call a rule -- and neither .stat nor a query can say so here (the
    // container carries variables, so the `=>` fact is a pattern and no
    // query answers it). The node count is what settles it.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto nodes = [&]
    {
        collector.clear();
        interactive.process(".stat");
        for (const auto& event : collector.events())
        {
            const std::string text = normalize(event.text);
            const auto        pos  = text.find("Nodes: ");
            if (pos != std::string::npos) return std::stoul(text.substr(pos + 7));
        }
        return 0UL;
    };

    interactive.process("{(X p Y) (X r Y)} => (X q Y)");
    const std::size_t after_first = nodes();

    interactive.process("{(A p B) (A r B)} => (A q B)");
    CHECK(nodes() == after_first);
}

TEST_CASE("rules: a rule has to be able to assert something")
{
    // The consequence side of the same question. A rule that cannot ASSERT
    // anything is not one: a container cannot be asserted -- it is not a
    // statement -- and neither can a bare name. Both were counted by .stat,
    // listed by .list-rules and taken by .remove-rules, and derived nothing
    // whatsoever.
    const char* inert[] = {
        "(X p Y) => {(X q Y)}",  // a set constant as the consequence
        "(X p Y) => @{(X q Y)}", // a collection
        "(X p Y) => flag"        // a bare name
    };

    for (const char* rule : inert)
    {
        CAPTURE(rule);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        interactive.process("a p b");
        interactive.process(rule);
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 0"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));
    }

    SUBCASE("what CAN be asserted still counts")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        // A statement, the contradiction marker, and a rule carrying one of
        // each next to a consequence that cannot be asserted -- one
        // assertable consequence is enough.
        process_lines(interactive, R"(
a p b
(X p Y) => (X q Y)
(X p Y) => !
)");
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 2"));
    }
}

TEST_CASE("rules: an untagged one-member condition survives a save/load round trip")
{
    // The reading is computed live from the graph rather than stored, so a
    // round trip is where it would break if anything about it were cached or
    // written down. It has to come back as a rule, fire, and explain.
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "zelph_single_condition.bin";
    std::filesystem::remove(file);

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("{(X p Y)} => (X q Y)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    std::filesystem::remove(file);

    collector.clear();
    interactive.process(".stat");
    CHECK(any_output_contains(collector, "Rules: 1"));

    interactive.process(".auto-run");
    interactive.process("a p b");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("S q O");
    CHECK(answers_contain(collector, "a q b"));

    collector.clear();
    interactive.process(".explain (a q b)");
    CHECK(any_output_contains(collector, "a p b"));
}
