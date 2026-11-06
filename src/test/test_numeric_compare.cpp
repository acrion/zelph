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

#include "numeric_compare_oracle.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Numeric comparison conditions: `A zelph:lt B`, `zelph:le`, `zelph:gt`,
// `zelph:ge`, `zelph:eq`, and `zelph:ne`. The engine processes them in the
// same manner as `!=` -- by evaluating a test between two bound terms, with no
// data stored or derived -- and orders every number according to its value,
// which is held to the verdict stdlib `cmp` derives wherever it derives a
// result (the line of values and the property cases at the end,
// numeric_compare_oracle.hpp). These conditions exist for data checks: one
// `cmp` fact per pair costs a run of the arithmetic rules and results in ten
// to sixteen derived facts for a pair of seven-digit decimals, and a check
// across a network of millions of dates necessitates millions of them.
// ---------------------------------------------------------------------------

namespace
{
    // One rule per relation, each deriving `(T is <relation>)` for the case T
    // where the two terms `T l A`, `T r B` meet the condition.
    void add_relation_rules(const zelph::console::Interactive& interactive)
    {
        for (const char* relation : zelph::network::numeric_relation_names)
            interactive.process(std::string("(T l A, T r B, A ") + relation + " B) => (T is " + std::string(relation).substr(6) + ")");
    }

    // The relations `(T is ...)` that hold for the case named `t`, in the order
    // lt le gt ge eq ne, as a single string: "lt le ne" for a case that is
    // less.
    std::string relations_of(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& t)
    {
        std::string held;
        for (const char* relation : zelph::network::numeric_relation_names)
        {
            const std::string name = std::string(relation).substr(6);
            if (janet_value(collector, interactive, "(zelph/exists \"" + t + "\" \"is\" \"" + name + "\")") == "true")
                held += (held.empty() ? "" : " ") + name;
        }
        return held;
    }

    // The names used for comparison that are associated with a node, expressed
    // as a single string.
    std::string comparison_nodes(const zelph::console::Interactive& interactive)
    {
        std::string named;
        for (const char* relation : zelph::network::numeric_relation_names)
            if (interactive.graph()->get_node(relation, "zelph", true) != 0) named += (named.empty() ? "" : " ") + std::string(relation);
        return named;
    }

    // How frequently the session has said that a comparison failed to locate a
    // digit alphabet.
    std::size_t no_alphabet_notices(const zelph::io::OutputCollector& collector)
    {
        return static_cast<std::size_t>(std::count_if(collector.events().begin(), collector.events().end(), [](const auto& e)
                                                      { return e.text.find("Numeric comparison: no digit alphabet is registered") != std::string::npos; }));
    }
} // namespace

TEST_CASE("numeric comparison: each relation holds exactly for its order")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
less l &39
less r &41
equal l &41
equal r &41
greater l &100
greater r &99
)");
        add_relation_rules(interactive);

        // The outcome of 39 versus 41 is decided by the most significant
        // digits, even though the least significant ones, 9 and 1, say the
        // opposite; 100 compared to 99 is resolved by length.
        CHECK(relations_of(collector, interactive, "less") == "lt le ne");
        CHECK(relations_of(collector, interactive, "equal") == "le ge eq");
        CHECK(relations_of(collector, interactive, "greater") == "gt ge ne"); });
}

// The condition functions as a test, not as a fact: a firing writes its
// consequence and nothing regarding the comparison, just as a firing through
// `!=` writes nothing about the guard. If a comparison had been stored, each
// check would have left one fact per pair in its wake -- the cost the condition
// was designed to avoid.
TEST_CASE("numeric comparison: no fact is stored or derived for the comparison")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &3
b v &5
(X v A, Y v B, A zelph:lt B) => (X below Y)
)");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "below" "b"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/mentioned (zelph/number "3") "zelph:lt" (zelph/number "5")))") == "false"); });
}

// Written as a statement, a comparison is a standard fact, just as `a != b`
// is: the line `&7 zelph:lt &6` asserts it, and a query asserts a comparison
// it writes between two constants, as it asserts any ground fact located
// outside a condition operator. A comparison condition that read such facts
// would hold for a false claim someone entered, and the query would respond
// with the fact its own construction stored. The REQUIREs establish the state
// the case concerns; they are not what is being tested.
TEST_CASE("numeric comparison: a comparison written as a statement is a fact no comparison condition reads")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &7
&7 zelph:lt &6
(X v A, A zelph:lt &6) => (X below six)
)");
        REQUIRE(janet_value(collector, interactive, R"((zelph/exists (zelph/number "7") "zelph:lt" (zelph/number "6")))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "below" "six"))") == "false");

        collector.clear();
        interactive.process("X v A, &9 zelph:lt &8");
        CHECK(collect_answers(collector).empty());
        REQUIRE(janet_value(collector, interactive, R"((zelph/exists (zelph/number "9") "zelph:lt" (zelph/number "8")))") == "true"); });
}

// A ground comparison can stand as the predicate in a statement, stating
// something about the comparison itself and nothing regarding what is being
// compared. Its terms were processed via parse_fact, whose objects also hold
// the facts that use the node as their predicate: next to
// `x (&3 zelph:lt &5) y` the rule was refused for writing two terms on a
// single side, and the identical statement over `&3 zelph:le &3` left the
// condition without a word, comparing &3 against the statement. .explain read
// the terms in the same way, and gave no derivation for what the rule had
// derived.
TEST_CASE("numeric comparison: a statement that uses a comparison as its predicate does not change what it compares")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
x (&3 zelph:lt &5) y
x (&3 zelph:le &3) y
a v &1
)");
        collector.clear();
        process_lines(interactive, R"(
(X v A, &3 zelph:lt &5) => (X ok-lt A)
(X v A, &3 zelph:le &3) => (X ok-le A)
)");
        CHECK_FALSE(any_output_contains(collector, "compares two terms"));

        collector.clear();
        interactive.process(".explain a ok-le &1");
        CHECK(any_output_contains(collector, "&3 zelph:le &3  [compared]"));

        CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "ok-lt" (zelph/number "1")))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "ok-le" (zelph/number "1")))") == "true"); });
}

// Engine vocabulary is established the moment a statement first names it, never
// at startup: introducing a single additional node during startup or within the
// math stack's imports causes a renumbering of every subsequent node and shifts
// the work of the Jacobian workload by as much as one percent. The registration
// integer-arithmetic.zph adds stands after its rules for the same reason.
TEST_CASE("numeric comparison: no comparison has a node before something names it")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    CHECK(comparison_nodes(interactive).empty());
    interactive.process(".import decimal-arithmetic");
    interactive.process(".import integer-arithmetic");
    CHECK(comparison_nodes(interactive).empty());

    interactive.process("(X v A, A zelph:gt &5) => (X big yes)");
    CHECK(comparison_nodes(interactive) == "zelph:gt");
}

// The pages on mathematics say that its entire stack consists of rules
// extending down to the digit tables, and they mean this literally: none of its
// modules writes a comparison condition, so no proof within that stack rests on
// one. A rule that wrote one would name it, and that name would correspond to a
// node. A module that began employing them would make those sentences false,
// and it fails here.
TEST_CASE("numeric comparison: no module of the mathematical stack names a comparison")
{
    for (const std::string module : {"decimal-arithmetic", "binary-arithmetic", "binary-nand-arithmetic", "integer-arithmetic", "primes", "primes-naf", "eml", "polynomial", "topoly", "symbolic-core", "symbolic-integers", "symbolic-minus", "symbolic-pow", "diff", "math-syntax", "math"})
    {
        INFO(module);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".import " + module);
        CHECK(comparison_nodes(interactive).empty());
    }
}

// The same applies to the worked example that runs the whole stack and
// introduces its own rules atop the existing modules.
TEST_CASE("numeric comparison: the Jacobian example names no comparison" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive on");
    interactive.process(".deductions off");
    interactive.process(".import examples/math/jacobian");
    CHECK(comparison_nodes(interactive).empty());
}

// They have no value to order, and a comparison is no place for an error: a
// data check executes across whatever the graph holds, numbers or otherwise.
TEST_CASE("numeric comparison: a term that is no number satisfies no relation, and raises no error")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        // Each case pairs a number with something that is none: an atom, a
        // list holding a letter (located in the least or the most
        // significant cell), a chain terminating at an atom rather than
        // nil, a statement.
        process_lines(interactive, R"(
atom l alice
atom r &1
letter-low l <1x>
letter-low r &1
letter-high l <x1>
letter-high r &1
improper l (1 cons x)
improper r &1
statement l (a p b)
statement r &1
)");
        collector.clear();
        add_relation_rules(interactive);
        // Here, and prior to the loop's execution: relations_of clears the
        // collector.
        CHECK_FALSE(any_event_contains(collector, "Error"));

        for (const std::string t : {"atom", "letter-low", "letter-high", "improper", "statement"})
        {
            INFO(t);
            CHECK(relations_of(collector, interactive, t).empty());
        } });
}

// Forms a user can type that are not a number, asked of the comparator on
// either side of a number and against itself: a failed decode on the right must
// carry the same meaning as one on the left, and a same-node shortcut, the
// obvious future optimisation, would make such a term equal to itself. Included
// among these are a cell or a signed term with several objects, a sign or a
// relation that is none, a magnitude that is not a numeral, and a chain broken
// by another predicate or by a tail lacking a statement. The signs are
// registered, or the signed forms would fail to be a number for the trivial
// reason. Asked directly and not via stdlib_cmp_oracle, which cannot take a
// term with several objects: it reads `cmp` through unification, and `cmp`
// derives a verdict for each more restricted reading of such a term.
TEST_CASE("numeric comparison: the comparator answers none for a form that is no number on either side and against itself")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
    interactive.process(".auto-run");

    const std::vector<std::string> forms = {"alice", "<1x>", "<x1>", "(1 cons x)", "(a p b)", "(1 cons nil <2>)", "(neg zint &5 &6)", "(neg other &5)", "(pos other &5)", "(foo zint &3)", "(neg zint alice)", "(neg zint <1x>)", "(1 cons (2 p nil))", "(1 cons {2 3})", "{1 2}"};

    std::vector<std::string> terms = forms;
    terms.push_back("&1");
    const std::vector<zelph::network::Node> nodes = nodes_of_terms(interactive, terms);

    zelph::network::Reasoning* const graph = interactive.graph();
    const zelph::network::Node       one   = nodes.back();
    const auto                       order = [&](const zelph::network::Node a, const zelph::network::Node b)
    { return numeric_order_text(zelph::network::compare_numbers(*graph, graph->number_vocabulary(), a, b)); };
    for (std::size_t i = 0; i < forms.size(); ++i)
    {
        INFO(forms[i]);
        CHECK(order(nodes[i], one) == "none");
        CHECK(order(one, nodes[i]) == "none");
        CHECK(order(nodes[i], nodes[i]) == "none");
    }
}

// A term featuring several objects possesses no single value, and reading the
// first would make the value rely on the order of a set. A narrower cell
// compares as usual, so it is the width that takes the number away and nothing
// else concerning these terms.
TEST_CASE("numeric comparison: a cell or a signed term with several objects satisfies no relation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
        process_lines(interactive, R"(
atom-tail l (1 cons nil x)
atom-tail r &3
two-tails l (1 cons nil (2 cons nil))
two-tails r &3
low-cell l (1 cons (2 cons nil x))
low-cell r &3
two-magnitudes l (neg zint &5 &7)
two-magnitudes r &3
narrow l (1 cons nil)
narrow r &3
)");
        add_relation_rules(interactive);
        for (const std::string t : {"atom-tail", "two-tails", "low-cell", "two-magnitudes"})
        {
            INFO(t);
            CHECK(relations_of(collector, interactive, t).empty());
        }
        CHECK(relations_of(collector, interactive, "narrow") == "lt le ne"); });
}

// What persists in a form is its value, so a signed zero decodes identically to
// zero. It formerly retained its sign, which no verdict reveals, since the
// order examines the digits first, and which the next reader of a DecodedNumber
// would have seen.
TEST_CASE("numeric comparison: a signed zero decodes like zero")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
    interactive.process(".auto-run");

    const std::vector<zelph::network::Node> nodes = nodes_of_terms(interactive, {"&0", "(neg zint &0)"});

    zelph::network::Reasoning* const graph = interactive.graph();
    zelph::network::DecodedNumber    zero;
    zelph::network::DecodedNumber    signed_zero;
    REQUIRE(zelph::network::decode_number(*graph, graph->number_vocabulary(), nodes[0], zero));
    REQUIRE(zelph::network::decode_number(*graph, graph->number_vocabulary(), nodes[1], signed_zero));
    CHECK(signed_zero.negative == zero.negative);
    CHECK(signed_zero.digits == zero.digits);
}

// The digits are what the loaded substrate registers, just as with the
// &-display. Absent one, <12> is merely a list of the two atoms 1 and 2, not
// a number, and any comparison of it must refrain from assuming a base. nil
// is read without any digit, so the missing alphabet is the sole factor
// preventing it from being zero.
TEST_CASE("numeric comparison: without a substrate nothing is a number")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
pair l <12>
pair r <34>
nothing l nil
nothing r nil
)");
        add_relation_rules(interactive);
        CHECK(relations_of(collector, interactive, "pair").empty());
        CHECK(relations_of(collector, interactive, "nothing").empty()); });
}

// A term that is not a number raises nothing, since a data check operates on
// whatever the graph holds. A session lacking a digit alphabet presents a
// different case: every number vanishes simultaneously, which a check
// interprets as "no violation". Thus, the first comparison evaluated says so,
// once, regardless of how many rules and bindings follow or how many lines
// later, and a module that brings the digits leaves nothing to say. A statement
// that holds no comparison evaluates none, and likewise a rule whose other
// conditions match nothing.
TEST_CASE("numeric comparison: with no digit alphabet the first comparison says so, once")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(T l A, T r B, A zelph:lt B) => (T is lt)");
        CHECK(no_alphabet_notices(collector) == 0);

        process_lines(interactive, R"(
first l <12>
first r <34>
second l <5>
second r <6>
(T l A, T r B, A zelph:ne B) => (T is ne)
)");
        CHECK(no_alphabet_notices(collector) == 1);
        CHECK_FALSE(any_event_contains(collector, "Error"));
        // A path forward that exists: `.import arithmetic` refers to no module.
        CHECK(any_event_contains(collector, "such as decimal-arithmetic"));

        interactive.process("third l <7>");
        CHECK(no_alphabet_notices(collector) == 1); });
}

// A `?` line runs a first pass that drops every diagnostic that is not a
// finding. A notice counted as said there would be permanently lost, and the
// check following it would read as "no violation".
TEST_CASE("numeric comparison: a question line does not swallow the notice")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    process_lines(interactive, R"(
a l <12>
a r <34>
(T l A, T r B, A zelph:lt B) => (T is lt)
)");
    collector.clear();
    interactive.process("? a l <12>");
    CHECK(no_alphabet_notices(collector) == 1);

    collector.clear();
    interactive.process(".run");
    CHECK(no_alphabet_notices(collector) == 0);
}

// When a module is loaded, the same rule has nothing to report, as it has not
// for a term that is not a number.
TEST_CASE("numeric comparison: with a digit alphabet nothing is said about one")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
number l &3
number r &5
word l alice
word r &5
)");
        add_relation_rules(interactive);
        CHECK(no_alphabet_notices(collector) == 0);

        interactive.process(".explain number is lt");
        CHECK(any_output_contains(collector, "&3 zelph:lt &5  [compared]"));
        CHECK(no_alphabet_notices(collector) == 0); });
}

// A load forgets the digits and the signs along with the graph they designated,
// while the rules brought by the file remain in force. A comparison within them
// then holds for nothing, whereas `cmp`, whose digit tables are facts from the
// file, continues to answer. Here is the state where a saved data check would
// read as "no violation", so the first comparison after the load says so, and
// the way out consists of importing the module once more. Nothing executed
// before the save, so what the rules derive here is derived after the load.
TEST_CASE("numeric comparison: after a load a comparison holds for nothing, and says so, until the module is imported again")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run");
    interactive.process(".import decimal-arithmetic");
    process_lines(interactive, R"(
a v &7
(X v A, A zelph:gt &5) => (X big yes)
(X v A) => (A cmp &5)
(X v A, A > &5) => (X big-by-cmp yes)
)");
    const std::string network = (std::filesystem::temp_directory_path() / "zelph_numeric_compare_load.bin").string();
    interactive.process(".save \"" + network + "\"");
    interactive.process(".load \"" + network + "\"");

    collector.clear();
    interactive.process(".run");
    CHECK(no_alphabet_notices(collector) == 1);
    CHECK_FALSE(any_event_contains(collector, "Error"));
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "big" "yes"))") == "false");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "big-by-cmp" "yes"))") == "true");

    // Re-loaded, so that the notice has nothing said to hide behind: with the
    // module imported once more, the comparison holds and reports nothing.
    interactive.process(".load \"" + network + "\"");
    std::filesystem::remove(network);
    interactive.process(".import decimal-arithmetic");
    collector.clear();
    interactive.process(".run");
    CHECK(no_alphabet_notices(collector) == 0);
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "big" "yes"))") == "true");
}

// The notice is stated only once, yet a load begins anew: the session might
// have issued it concerning the graph it previously held, and anyone who loads
// a network and runs its checks must receive it anew, as the report of an
// unusable net is reiterated.
TEST_CASE("numeric comparison: a load that replaces the graph says it again")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_numeric_compare_again.bin";
    run_both_modes([&file](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a l <12>
a r <34>
(T l A, T r B, A zelph:lt B) => (T is lt)
)");
        REQUIRE(no_alphabet_notices(collector) == 1);
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        collector.clear();
        interactive.process(".run");
        CHECK(no_alphabet_notices(collector) == 1); });
    std::filesystem::remove(file);
}

// A term whose subject is its own predicate enables a comparison to read back
// without a structure, provided the graph comes from a file or the fact stores
// are off. Such a term is not a number, like any statement, and a data check
// processes whatever the graph holds: it fulfils no relation and raises no
// error. The refusal stays for what could genuinely be written wrong, several
// objects.
TEST_CASE("numeric comparison: a term whose subject is its own predicate is no number after a load and without a store too")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    SUBCASE("without a store") { interactive.process(".fact-stores off"); }
    SUBCASE("with a store, after a load")
    {
        interactive.process(".auto-run");
        process_lines(interactive, R"(
q go w
(q go W, (foo foo bar) zelph:ge baz) => (W holds yes)
)");
        const std::string network = (std::filesystem::temp_directory_path() / "zelph_numeric_compare_self_predicate.bin").string();
        interactive.process(".save \"" + network + "\"");
        interactive.process(".load \"" + network + "\"");
        std::filesystem::remove(network);
        interactive.process(".import decimal-arithmetic");
    }
    process_lines(interactive, R"(
q go w
(q go W, (foo foo bar) zelph:ge baz) => (W holds yes)
)");
    interactive.process(".run");
    CHECK_FALSE(any_event_contains(collector, "compares two terms"));
    CHECK(janet_value(collector, interactive, R"((zelph/exists "w" "holds" "yes"))") == "false");

    process_lines(interactive, "(q go W, &3 zelph:ge &1 &2) => (W several yes)\n");
    interactive.process(".run");
    CHECK(any_event_contains(collector, "compares two terms"));
}

// The signs go with the digits, and the module responsible for their
// registration differs from the one that brings the digits back: with merely
// the substrate re-imported, (neg zint &5) functions as a statement just like
// any other. The import tests import integer-arithmetic again as well, which
// would conceal a registration that survived.
TEST_CASE("numeric comparison: after a load with only the substrate imported again a signed term is no number")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    interactive.process(".import integer-arithmetic");
    const std::string network = (std::filesystem::temp_directory_path() / "zelph_numeric_compare_signs_load.bin").string();
    interactive.process(".save \"" + network + "\"");
    interactive.process(".load \"" + network + "\"");
    std::filesystem::remove(network);

    interactive.process(".import decimal-arithmetic");
    process_lines(interactive, R"(
s v (neg zint &5)
(X v A, A zelph:lt &0) => (X negative yes)
)");
    interactive.process(".run");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "s" "negative" "yes"))") == "false");
}

// The comparison reads its digits via the registration that turns the
// &-display on, and no other mechanism informs it about what constitutes a
// digit. With an empty alphabet, no number remains, not even nil, which is read
// without a digit, whereas `cmp`, whose digit tables consist of facts,
// continues to answer. Once digits are re-registered, the numbers return.
TEST_CASE("numeric comparison: an empty digit alphabet leaves no number to compare, and registering it again restores them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process("%(zelph/set-number-digits [])");
        process_lines(interactive, R"(
pair l &3
pair r &5
nothing l nil
nothing r nil
)");
        add_relation_rules(interactive);
        CHECK(no_alphabet_notices(collector) == 1);
        CHECK(relations_of(collector, interactive, "pair").empty());
        CHECK(relations_of(collector, interactive, "nothing").empty());

        interactive.process("&3 cmp &5");
        CHECK(janet_value(collector, interactive, R"((zelph/exists (zelph/number "3") "<" (zelph/number "5")))") == "true");

        interactive.process("%(zelph/set-number-digits (map string (range 10)))");
        interactive.process(".run");
        CHECK(relations_of(collector, interactive, "pair") == "lt le ne");
        CHECK(relations_of(collector, interactive, "nothing") == "le ge eq"); });
}

// What the stdlib `cmp` reads as a number, the condition processes
// identically: the list featuring leading zeros that subtraction derives
// (<007>), and nil, the exhausted list that every digit recursion ends at,
// which `cmp` treats as zero.
TEST_CASE("numeric comparison: leading zeros and nil compare by value, as cmp compares them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
zeros l <007>
zeros r &7
padded l <0010>
padded r &9
empty l nil
empty r &0
empty-less l nil
empty-less r &1
)");
        add_relation_rules(interactive);
        CHECK(relations_of(collector, interactive, "zeros") == "le ge eq");
        CHECK(relations_of(collector, interactive, "padded") == "gt ge ne");
        CHECK(relations_of(collector, interactive, "empty") == "le ge eq");
        CHECK(relations_of(collector, interactive, "empty-less") == "lt le ne"); });
}

// The signed integers from integer-arithmetic.zph, which registers their form
// via (zelph/set-number-signs). Each carries a value: (neg zint N) represents
// the negative of N, and (pos zint N) stands for N itself, and the condition
// orders by that value whether or not the facade permits `cmp` to respond: a
// natural compared to the operand form (pos zint N) gets routed nowhere, and
// neither does nil compared to a signed term, or a signed term whose magnitude
// is the empty list, as nil does not meet the facade's numeral test. The last
// two cases are terms that are not integers in any sense: a term under the sign
// relation where the sign is not a valid sign, and a signed term whose
// magnitude is not a numeral. The line of values below holds these forms to
// their values and to `cmp` outside check mode; check mode under the facade's
// rules is the reason this case exists.
TEST_CASE("numeric comparison: signed integers compare by value" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".import integer-arithmetic");
        process_lines(interactive, R"(
two-negatives l (neg zint &5)
two-negatives r (neg zint &3)
mixed l &3
mixed r (neg zint &5)
signed-zero l (neg zint &0)
signed-zero r &0
operand-form l (pos zint &3)
operand-form r (neg zint &5)
natural-vs-operand-form l &5
natural-vs-operand-form r (pos zint &3)
nil-vs-negative l nil
nil-vs-negative r (neg zint &5)
nil-magnitude l (neg zint nil)
nil-magnitude r &0
odd-sign l (foo zint &3)
odd-sign r &1
odd-magnitude l (neg zint alice)
odd-magnitude r &1
)");
        add_relation_rules(interactive);
        CHECK(relations_of(collector, interactive, "two-negatives") == "lt le ne");
        CHECK(relations_of(collector, interactive, "mixed") == "gt ge ne");
        CHECK(relations_of(collector, interactive, "signed-zero") == "le ge eq");
        CHECK(relations_of(collector, interactive, "operand-form") == "gt ge ne");
        CHECK(relations_of(collector, interactive, "natural-vs-operand-form") == "gt ge ne");
        CHECK(relations_of(collector, interactive, "nil-vs-negative") == "gt ge ne");
        CHECK(relations_of(collector, interactive, "nil-magnitude") == "le ge eq");
        CHECK(relations_of(collector, interactive, "odd-sign").empty());
        CHECK(relations_of(collector, interactive, "odd-magnitude").empty()); });
}

// Which pairs of signed and unsigned terms `cmp` compares is stated by the
// rules of integer-arithmetic.zph's facade and by nothing within the engine,
// which orders every pair by value. Thus, a single line through the values is
// held to the comparator, the six conditions, and `cmp`, across all ordered
// pairs: each group holds the forms corresponding to one value, the groups
// ascend, and the order of a pair is determined by the order of its two groups,
// which no engine has seen. A line closes the shapes by construction, where a
// list of pairs holds those that someone has conceived. It gives the total
// order upon which sorting will depend: every pair receives a verdict, the
// reverse pair receives the reverse verdict, and the verdicts align with one
// line. It binds `cmp` across every shape in the selection that the packages
// run, where an incorrect verdict from the facade, such as (pos zint A) versus
// (pos zint B), previously allowed the entire suite to pass. The pairs that
// `cmp` does not address remain among them: these are what the order adds. The
// terms that are not numbers stand on either side of the numbers and against
// themselves, now under the facade's rules, where `cmp` must not provide an
// answer for them either. One run of each and outside check mode, the case
// stays cheap (approximately 0.6 s).
TEST_CASE("numeric comparison: one line of values orders every pair and agrees with cmp where it answers")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".semi-naive on");
    interactive.process(".deductions off");
    interactive.process(".import decimal-arithmetic");
    interactive.process(".import integer-arithmetic");

    using Order = zelph::network::NumericOrder;

    const std::vector<std::vector<std::string>> ascending = {
        {"(neg zint &91)"},
        {"(neg zint &39)", "(neg zint <039>)"},
        {"&0", "<00>", "nil", "(neg zint &0)", "(pos zint &0)", "(neg zint nil)", "(pos zint nil)"},
        {"&3", "<03>", "(pos zint &3)", "(pos zint <03>)"},
        {"&39", "(pos zint &39)"},
        {"&91"},
    };

    std::vector<NumericCase> cases;
    for (std::size_t g = 0; g < ascending.size(); ++g)
        for (std::size_t h = 0; h < ascending.size(); ++h)
        {
            const Order by_value = g < h ? Order::less : (g == h ? Order::equal : Order::greater);
            for (const std::string& a : ascending[g])
                for (const std::string& b : ascending[h])
                    cases.push_back({a, b, by_value});
        }
    for (const char* const term : {"(foo zint &3)", "(neg zint alice)", "alice"})
    {
        for (const std::vector<std::string>& group : ascending)
            for (const std::string& number : group)
            {
                cases.push_back({term, number, Order::none});
                cases.push_back({number, term, Order::none});
            }
        cases.push_back({term, term, Order::none});
    }
    const std::vector<NumericOracleEntry> oracle = stdlib_cmp_oracle(interactive, cases);

    check_every_verdict_occurs(oracle);

    zelph::network::Reasoning* const graph      = interactive.graph();
    const auto                       comparator = [&](const zelph::network::Node a, const zelph::network::Node b)
    { return zelph::network::compare_numbers(*graph, graph->number_vocabulary(), a, b); };
    check_against_stdlib(oracle, comparator);
    check_against_values(oracle, comparator);
    check_conditions_against_values(interactive, oracle);
}

// Without integer-arithmetic, there is no signed form, and `cmp` compares
// natural lists exclusively: (neg zint &5) constitutes a statement just like
// any other.
TEST_CASE("numeric comparison: a signed term is no number while nothing registered the signs")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
unregistered l (neg zint &5)
unregistered r &3
)");
        add_relation_rules(interactive);
        CHECK(relations_of(collector, interactive, "unregistered").empty()); });
}

// zelph/set-number-signs is the method a module uses to declare its signed
// form, and the sole means through which the comparison acquires this
// knowledge. The cases below call it as a script would, without
// integer-arithmetic, whose own invocation is addressed in the import
// tests. Without any argument, it clears the registration, and a signed
// term then functions as a statement just like any other; upon
// re-registration, the identical term becomes a number once more. Each case
// maintains its own pair: a derived relation persists even when the
// registration is removed.
TEST_CASE("zelph/set-number-signs: no argument clears the signed form, and registering it again restores it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
        process_lines(interactive, R"(
registered l (neg zint &5)
registered r &0
)");
        add_relation_rules(interactive);
        CHECK(relations_of(collector, interactive, "registered") == "lt le ne");

        interactive.process("%(zelph/set-number-signs)");
        process_lines(interactive, R"(
cleared l (neg zint &7)
cleared r &0
)");
        CHECK(relations_of(collector, interactive, "cleared").empty());

        interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
        interactive.process(".run");
        CHECK(relations_of(collector, interactive, "cleared") == "lt le ne"); });
}

// A signed number needs the relation and both signs, necessitating three
// different nodes for them: with two nodes, or with one node serving as both
// signs, a term could not say which sign it carries. A refused call leaves
// the registration in force as it was.
TEST_CASE("zelph/set-number-signs: two arguments, or a node in two roles, are refused, and the registration stays")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");

    CHECK_THROWS_WITH_AS(interactive.process(R"(%(zelph/set-number-signs "zint" "pos"))"),
                         doctest::Contains("zelph/set-number-signs: expected the sign relation and the positive and the negative sign, or no argument to clear, got 2 argument(s)"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "pos"))"),
                         doctest::Contains("the sign relation and the two signs must be three different nodes"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(interactive.process(R"(%(zelph/set-number-signs "zint" "zint" "neg"))"),
                         doctest::Contains("must be three different nodes"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "zint"))"),
                         doctest::Contains("must be three different nodes"),
                         std::runtime_error);

    process_lines(interactive, R"(
kept l (neg zint &5)
kept r &0
)");
    add_relation_rules(interactive);
    CHECK(relations_of(collector, interactive, "kept") == "lt le ne");
}

// Names are resolved within the current language, just as digits are: under
// `.lang de`, the three names refer to nodes in `de`, not to the ones
// `(neg zint &5)` is written with under `.lang zelph`, so a registration by
// name there outlines a different form. The nodes themselves can be passed
// from any language.
TEST_CASE("zelph/set-number-signs: names resolve in the current language, nodes in every one")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    process_lines(interactive, R"(
signed l (neg zint &5)
signed r &0
)");
    add_relation_rules(interactive);

    interactive.process(".lang de");
    interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
    interactive.process(".lang zelph");
    interactive.process(".run");
    CHECK(relations_of(collector, interactive, "signed").empty());

    interactive.process(".lang de");
    interactive.process(R"(%(zelph/set-number-signs (zelph/resolve "zint" "zelph") (zelph/resolve "pos" "zelph") (zelph/resolve "neg" "zelph")))");
    interactive.process(".lang zelph");
    interactive.process(".run");
    CHECK(relations_of(collector, interactive, "signed") == "lt le ne");
}

// `.name seven 7` merges the node that holds the name into the node that is
// given it, with the holder potentially being a registered digit. The
// registration names its nodes by id, whereas the `cmp` rules are rehashed
// following the merge and continue to answer. Left behind, the comparison read
// every number constructed from the digit as none, and the &-display printed it
// as a plain list. The number comprises two digits, so that the base shows as
// well: the table's size is the base, and a table retaining the old key beside
// the new one read &17 as &18.
TEST_CASE("numeric comparison: a name merge that removes a registered digit hands the registration to the survivor")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    process_lines(interactive, R"(
a v &17
seven p q
.name seven 7
c v &6
(X v A, Y v B, A zelph:lt B) => (X below Y)
)");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "c" "below" "a"))") == "true");

    collector.clear();
    interactive.process("a v X");
    CHECK(answers_contain(collector, "a v &17"));
}

// The same applies to a sign: (neg zint &5) ceased to be a number following the
// merge, yet `cmp` continued to compare it. What the merge rebuilt and what is
// entered afterwards are both read through the survivor.
TEST_CASE("numeric comparison: a name merge that removes a registered sign hands the registration to the survivor")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    interactive.process(".import integer-arithmetic");
    process_lines(interactive, R"(
a v (neg zint &5)
b v &3
minus p q
.name minus neg
c v (neg zint &7)
(X v A, Y v B, A zelph:lt B) => (X below Y)
)");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "below" "b"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "c" "below" "a"))") == "true");
}

// The sign relation and the positive sign are registered nodes too, each
// carrying its own identifier for transfer: a term constructed from either is
// interpreted via the survivor, just as one built from the negative sign is.
TEST_CASE("numeric comparison: a name merge of the sign relation or the positive sign hands the registration to the survivor")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    interactive.process(".import integer-arithmetic");
    process_lines(interactive, R"(
a v (neg zint &5)
b v &3
c v (pos zint &7)
rel p q
.name rel zint
plus p q
.name plus pos
(X v A, Y v B, A zelph:lt B) => (X below Y)
)");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "below" "b"))") == "true");
    CHECK(janet_value(collector, interactive, R"((zelph/exists "b" "below" "c"))") == "true");
}

// Two digits merged into a single digit: the survivor keeps its original value,
// and the digit that was absorbed remains in the table under its prior key,
// since the table's size is the base. A table simply rekeyed lost an entry,
// thus &34 appeared as &31 in base 9, and which of the two values survived
// depended on the table's iteration order: &12, whose two digits now form one
// node, displayed as &20.
TEST_CASE("numeric comparison: a name merge of two registered digits keeps the base and the survivor's value")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import decimal-arithmetic");
    process_lines(interactive, R"(
b v &12
.name 1 2
)");
    collector.clear();
    interactive.process("c v &34");
    CHECK(any_output_contains(collector, "c v &34"));

    collector.clear();
    interactive.process("b v X");
    CHECK(answers_contain(collector, "b v &11"));
}

// The plan puts the comparison after the conditions that bind its terms,
// regardless of where it appears, just as it does with `!=`; in a seeded
// run, the seed's bindings come first. If written prior to them and
// evaluated beforehand, it would have nothing to compare.
TEST_CASE("numeric comparison: it runs after the conditions that bind its terms, wherever it is written")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
(A zelph:lt B, X v A, Y v B) => (X below Y)
a v &3
b v &5
)");
        CHECK(any_output_contains(collector, "(a below b)"));
        CHECK_FALSE(any_output_contains(collector, "(b below a)"));
        CHECK_FALSE(any_event_contains(collector, "Error")); });
}

// No condition binds B. `!=` would remember such a guard for a later
// condition to resolve, resulting in no match by the end; a comparison
// retains nothing, as the plan positions it after every condition that might
// bind a term. The rule's shape leaves B open regardless of the binding, so
// the refusal is said once -- and not once per fact of `v`.
TEST_CASE("numeric comparison: a term no condition binds is refused, once")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &3
b v &5
c v &7
)");
        collector.clear();
        interactive.process("(X v A, A zelph:lt B) => (X below B)");

        CHECK(count_outputs_containing(collector, "no condition binds \"B\"") == 1);
        for (const char* x : {"(a below", "(b below", "(c below"})
            CHECK_FALSE(any_output_contains(collector, x)); });
}

// The open variable may reside within a structure, and its openness does not
// depend on the facts, just as the comparison does not: `<44>`, the internal
// component of the second term, lacks a node here, yet the term remains open
// and is not merely a term that no fact denotes.
TEST_CASE("numeric comparison: a variable no condition binds is refused inside a structure too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(R"(%(zelph/set-number-signs "zint" "pos" "neg"))");
        process_lines(interactive, R"(
a v &3
b d 4
)");
        collector.clear();
        interactive.process("(X v A, (neg zint B) zelph:lt &0) => (X below B)");
        CHECK(count_outputs_containing(collector, "no condition binds \"neg zint B\"") == 1);

        collector.clear();
        interactive.process("(X d D, ((D cons (D cons nil)) cons C) zelph:lt &0) => (X hit C)");
        CHECK(count_outputs_containing(collector, "no condition binds") == 1); });
}

// A query that has no further means to bind its terms meets the same
// refusal. An answer lists the query's conditions in the order of its
// condition set, which aligns with node ids, thus each answer is
// verified part by part.
TEST_CASE("numeric comparison: a query compares under the bindings of its other conditions")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &3
b v &5
c v &7
)");
        collector.clear();
        interactive.process("X v A, A zelph:gt &4");
        const std::vector<std::string> answers = collect_answers(collector);
        const auto                     answered = [&](const std::string& fact, const std::string& comparison)
        {
            return std::any_of(answers.begin(), answers.end(), [&](const std::string& a)
                               { return a.find(fact) != std::string::npos && a.find(comparison) != std::string::npos; });
        };
        CHECK(answered("b v &5", "&5 zelph:gt &4"));
        CHECK(answered("c v &7", "&7 zelph:gt &4"));
        CHECK(answers.size() == 2);

        collector.clear();
        interactive.process("A zelph:lt B");
        CHECK(collect_answers(collector).empty());
        CHECK(any_output_contains(collector, "no condition binds")); });
}

// A term can be a structure whose variables are bound by the other conditions:
// (A cons R) in a rule operating on the cells of a list reads as the
// numeral it is.
TEST_CASE("numeric comparison: a structured term is read under the bindings")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
small v &12
large v &98
(X v (A cons R), (A cons R) zelph:gt &50) => (X over fifty)
)");
        CHECK(any_output_contains(collector, "(large over fifty)"));
        CHECK_FALSE(any_output_contains(collector, "(small over fifty)")); });
}

// A value is attributed to a number regardless of whether the graph holds the
// statement that writes it, thus the comparison requires no fact: -3 is less
// than zero even though no statement asserts (neg zint &3). It generates
// nothing either, which enables a check to process many pairs without
// necessitating a node for each. A term that is not a number under its bindings
// fails, just as a variable bound to one does.
TEST_CASE("numeric comparison: a structured term needs no fact and creates none")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".import integer-arithmetic");
        process_lines(interactive, R"(
a v &3
b v alice
(X v A, (neg zint A) zelph:lt &0) => (X below-zero yes)
)");
        CHECK_FALSE(any_event_contains(collector, "Error"));
        CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "below-zero" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "b" "below-zero" "yes"))") == "false");
        CHECK(janet_value(collector, interactive, R"((zelph/mentioned "neg" "zint" (zelph/number "3")))") == "false"); });
}

// Reading a term via a node would make the comparison rely on the fact that node
// is, whereas seeding and the negation levels correctly regard a comparison as a
// test that reads none: a rule writing the term in the same run would come too
// late for the seeded evaluation, and check mode would stop with a completeness
// violation. Read by value, nothing depends on when the term arrives, or on
// whether it arrives at all, so every mode derives this in the first run, even
// when the rule that writes the term is positioned at a later negation level
// than the comparison.
TEST_CASE("numeric comparison: a structured term does not depend on when its fact arrives")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".import integer-arithmetic");
        interactive.process(".auto-run");

        SUBCASE("a rule writes the term in the same run")
        {
            process_lines(interactive, R"(
(X v A, (neg zint A) zelph:lt &9) => (X below A)
(X v A) => (X w (neg zint A))
a v &3
(X d D, (D cons nil) zelph:lt &5) => (X small yes)
(X d D) => (D cons nil)
x d 4
)");
            interactive.process(".run");
            CHECK(janet_value(collector, interactive, R"((zelph/exists "a" "below" (zelph/number "3")))") == "true");
            CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "small" "yes"))") == "true");
        }
        SUBCASE("the rule that writes the term stands at a later level")
        {
            process_lines(interactive, R"(
(X z yes, ¬(X n yes)) => (X m yes)
(X v A, ¬(X m yes)) => (neg zint A)
(X v A, ¬(X halt yes), (neg zint A) zelph:lt &0) => (X below yes)
x v &5
)");
            interactive.process(".run");
            CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "below" "yes"))") == "true");
        } });
}

// Each component of a structure may originate from a binding, and the value is
// the one the explicitly written term possesses: a cell within a list, nested;
// the cons relation and the sign relation; the sign itself; a magnitude in any
// form, or even a structure that holds a variable. What a structure holds is
// interpreted with the same rigour as what a node holds: a part that is not a
// digit, a bound relation or sign that is none, and a term comprising several
// objects make it not a number.
TEST_CASE("numeric comparison: every part of a structured term can come from a binding")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".import integer-arithmetic");
        process_lines(interactive, R"(
x d 3
x t 5
x s neg
x r cons
x m <05>
x z zint
x o foo
y d alice
z d 3
z r p
(X d D, X t T, (D cons (T cons nil)) zelph:eq &53) => (X nested yes)
(X d D, X r P, (D P nil) zelph:lt &5) => (X relation yes)
(X d D, &5 zelph:gt (D cons nil)) => (X right yes)
(X s S, (S zint &7) zelph:lt &0) => (X sign yes)
(X m M, (neg zint M) zelph:eq (neg zint &5)) => (X magnitude yes)
(X d D, (D cons nil <2>) zelph:lt &5) => (X wide yes)
(X z R, (neg R &5) zelph:lt &0) => (X sign-relation yes)
(X o S, (S zint &7) zelph:lt &9) => (X odd-sign yes)
(X d D, (neg zint (D cons nil)) zelph:lt &0) => (X inner-magnitude yes)
)");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "nested" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "relation" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "right" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "sign" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "magnitude" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "sign-relation" "yes"))") == "true");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "inner-magnitude" "yes"))") == "true");

        CHECK(janet_value(collector, interactive, R"((zelph/exists "y" "right" "yes"))") == "false");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "z" "relation" "yes"))") == "false");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "odd-sign" "yes"))") == "false");
        CHECK(janet_value(collector, interactive, R"((zelph/exists "x" "wide" "yes"))") == "false"); });
}

// For two numbers, the negation of a comparison yields the complementary
// relation, which is expressible. `¬` would add only those terms that are
// not numbers -- `¬(A zelph:lt B)` holding for a list of letters -- a
// reading no data check wants, and the opposite of SPARQL's `!` over a
// comparison, which raises an error for incomparable terms. Thus, it is
// refused as `¬(X != Y)` is, at evaluation, naming the relation to write.
TEST_CASE("numeric comparison: ¬ is refused and names the complementary relation")
{
    const std::vector<std::pair<std::string, std::string>> complements = {
        {"zelph:lt", "zelph:ge"}, {"zelph:le", "zelph:gt"}, {"zelph:gt", "zelph:le"}, {"zelph:ge", "zelph:lt"}, {"zelph:eq", "zelph:ne"}, {"zelph:ne", "zelph:eq"}};

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &3
b v &5
)");
        for (const auto& [relation, complement] : complements)
        {
            INFO(relation);
            collector.clear();
            interactive.process("(X v A, Y v B, ¬(A " + relation + " B)) => (X not-" + relation.substr(6) + " Y)");
            CHECK(any_output_contains(collector, "¬ cannot be applied to \"" + relation + "\" -- write \"" + complement + "\" instead"));
            CHECK_FALSE(any_output_contains(collector, "(a not-"));
            CHECK_FALSE(any_output_contains(collector, "(b not-"));
        } });
}

// The deduction line names the condition under its bindings, much like it
// names a `!=` guard. A number compared to itself is rendered in verbose form:
// the self-fact sugar would print `:zelph:eq &5`, a marker that nobody
// established. Verified against the printed text -- the sugar-aware helpers
// take both spellings as equivalent.
TEST_CASE("numeric comparison: the deduction line names the comparison, a number compared with itself verbose")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &5
b v &5
c v &9
(X v A, Y v B, A zelph:lt B) => (X below Y)
(X v A, Y v B, X != Y, A zelph:eq B) => (X level Y)
)");
        CHECK(any_event_contains(collector, "(&5 zelph:lt &9)"));
        CHECK(any_event_contains(collector, "(&5 zelph:eq &5)"));
        CHECK_FALSE(any_event_contains(collector, ":zelph:eq")); });
}

// A name in a different language refers to the same comparison, and it is still
// not a marker. The test examined the name within the current language, so
// under the language of `gleich`, every print utilized the self-fact sugar,
// `:gleich &5`.
TEST_CASE("numeric comparison: a number compared with itself prints verbose under a name of another language")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
.name zelph:eq de gleich
.lang de
a v &5
b v &5
(X v A, Y v B, X != Y, A gleich B) => (X level Y)
)");
        CHECK(any_event_contains(collector, "(&5 gleich &5)"));
        CHECK_FALSE(any_event_contains(collector, ":gleich")); });
}

// What made the premise hold is the engine's test, not a fact any
// individual asserted, so the tree says [compared], not [axiom] -- for
// the reason a walk says [closure].
TEST_CASE("numeric comparison: .explain prints the satisfied comparison as a [compared] premise")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
alice born &1950
bob born &1960
(X born A, Y born B, A zelph:lt B) => (X older-than Y)
)");
        collector.clear();
        interactive.process(".explain alice older-than bob");
        CHECK(any_output_contains(collector, "&1950 zelph:lt &1960  [compared]"));
        CHECK(any_output_contains(collector, "alice born &1950  [axiom]"));
        CHECK_FALSE(any_output_contains(collector, "no derivation found")); });
}

// The proof as data carries the comparisons as well, under a key distinct from
// :walked.
TEST_CASE("numeric comparison: zelph/explain lists the comparison under :compared")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
alice born &1950
bob born &1960
(X born A, Y born B, A zelph:lt B) => (X older-than Y)
)");
        CHECK(janet_value(collector, interactive, R"((length ((zelph/explain "alice" "older-than" "bob") :compared)))") == "1");
        CHECK(janet_value(collector, interactive, R"((length ((zelph/explain "alice" "older-than" "bob") :walked)))") == "0");
        // A step lacking a comparison does not include a :compared, so the
        // printed proofs of every other rule read as they previously did.
        CHECK(janet_value(collector, interactive, R"((((zelph/explain "alice" "older-than" "bob") :premises) 0))") .find(":compared") == std::string::npos); });
}

// The backward search tests the comparison in the same manner as the forward
// pass. A fact that the rule's consequence matches, yet for which the
// comparison fails, is not justified by the rule -- read as a plain
// condition, it would have matched no fact and been discarded regardless, so
// the relevant case is the one that holds (above); this one pins that a
// failing comparison is not overlooked.
TEST_CASE("numeric comparison: .explain gives no derivation through a comparison that fails")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
alice born &1970
bob born &1960
(X born A, Y born B, A zelph:lt B) => (X older-than Y)
alice older-than bob
)");
        collector.clear();
        interactive.process(".explain alice older-than bob");
        CHECK(any_output_contains(collector, "[asserted; no derivation found]"));
        CHECK_FALSE(any_output_contains(collector, "[compared]")); });
}

// A rule that negates a comparison is refused by the forward pass and never
// fires. The backward search must not treat it as a justification: read as
// a negated plain condition, `¬(A zelph:lt B)` finds no fact (no comparison
// is stored), holds true, and would falsely account for a fact via a rule
// that cannot have derived it.
TEST_CASE("numeric comparison: .explain takes no justification from a rule that negates a comparison")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &5
b v &3
(X v A, Y v B, ¬(A zelph:lt B)) => (X notless Y)
a notless b
)");
        collector.clear();
        interactive.process(".explain a notless b");
        CHECK(any_output_contains(collector, "[asserted; no derivation found]"));
        CHECK_FALSE(any_output_contains(collector, "[absent]")); });
}

// No condition binds A, so the forward pass refuses the rule regardless of the
// binding and derives nothing. The fact is merely asserted, and the head of the
// rule had A bound before the backward search tested the comparison, which then
// held and gave the fact a [compared] proof via a rule that was never fired.
TEST_CASE("numeric comparison: .explain takes no justification from a rule whose comparison only the head binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a v &1
(a v C, A zelph:lt &5) => (A small C)
&3 small &1
)");
        collector.clear();
        interactive.process(".explain &3 small &1");
        CHECK(any_output_contains(collector, "[asserted; no derivation found]"));
        CHECK_FALSE(any_output_contains(collector, "[compared]")); });
}

// On the opposite side of the scenario described above: the members of a set
// pattern are bound through pairing, and collect_variables does not examine
// them within a rule's text. The forward pass derives this fact, so a reading
// that omitted them would take its proof away.
TEST_CASE("numeric comparison: .explain still justifies a comparison whose terms a set pattern binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
x has {&1 &3}
(x has {A B}, A zelph:lt B) => (A before B)
)");
        collector.clear();
        interactive.process(".explain &1 before &3");
        CHECK(any_output_contains(collector, "&1 zelph:lt &3  [compared]")); });
}

// The third method by which a term gets bound: a path condition binds it, and
// the forward pass derives the fact, so in this case as well, a reading that
// left the condition out would take the proof away.
TEST_CASE("numeric comparison: .explain still justifies a comparison whose term a path condition binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
a sub &1
&1 sub &3
(a sub⁺ Y, Y zelph:lt &4) => (Y under four)
)");
        collector.clear();
        interactive.process(".explain &3 under four");
        CHECK(any_output_contains(collector, "&3 zelph:lt &4  [compared]")); });
}

// The backward search reads a term in the same manner as the forward pass, so a
// term that the graph does not hold explains just like any other, and the same
// value determines the outcome: (neg zint &0) equals zero, which no comparison
// puts below zero.
TEST_CASE("numeric comparison: .explain reads a structured term by value as well")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".import integer-arithmetic");
        process_lines(interactive, R"(
a v &3
b v &0
(X v A, (neg zint A) zelph:lt &0) => (X below-zero yes)
b below-zero yes
)");
        collector.clear();
        interactive.process(".explain a below-zero yes");
        CHECK(any_output_contains(collector, "(neg zint &3) zelph:lt &0  [compared]"));
        CHECK(janet_value(collector, interactive, R"((zelph/mentioned "neg" "zint" (zelph/number "3")))") == "false");

        collector.clear();
        interactive.process(".explain b below-zero yes");
        CHECK(any_output_contains(collector, "[asserted; no derivation found]")); });
}

// The six names are the engine's in the zelph language, as `closure` and
// `refuted` are: when assigned to another node, a name would make that
// node serve as a comparison, and when removed from its own, it would
// end every comparison written using it.
TEST_CASE("numeric comparison: the names belong to the engine's vocabulary")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("(X v A, Y v B, A zelph:lt B) => (X below Y)");
    interactive.process("other v one");

    CHECK_THROWS_WITH_AS(interactive.process(".name other zelph:le"), doctest::Contains("Requested name 'zelph:le' belongs to the engine's vocabulary in language 'zelph'"), std::runtime_error);
    CHECK_THROWS_WITH_AS(interactive.process(".name zelph:lt smaller"), doctest::Contains("A name of the engine's vocabulary cannot be taken from its node."), std::runtime_error);
}

// A rule formulated using a single comparison places it within the guards, as
// the level analysis reads it: a condition that tests bound terms and does not
// read any fact.
TEST_CASE("numeric comparison: zelph/rules lists a comparison among the guards")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("(X v A, Y v B, A zelph:ge B) => (X atleast Y)");

    CHECK(janet_value(collector, interactive, "(length ((first (zelph/rules)) :guards))") == "1");
    CHECK(janet_value(collector, interactive, "(length ((first (zelph/rules)) :conditions))") == "2");
    CHECK(janet_value(collector, interactive, "(string (first ((first (zelph/rules)) :guards)))") == "\"A zelph:ge B\"");
}

// A rule that writes a rule rebuilds a comparison akin to any other
// condition within the rule it produces, with the generator's binding
// substituted: a threshold stated as a fact transforms into the constant
// of a check.
TEST_CASE("numeric comparison: a rule generator writes a comparison like any other condition")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        process_lines(interactive, R"(
(T is threshold) => ((X v A, A zelph:gt T) => (X over T))
a v &7
b v &3
&5 is threshold
)");
        CHECK(any_output_contains(collector, "(a over &5)"));
        CHECK_FALSE(any_output_contains(collector, "(b over &5)")); });
}

// A condition whose subject is its predicate, `P P Z`, lacks a single
// relation: the reading of relation types leaves the subject out, and the
// level analysis reads 0 for it. A comparison still without a node also
// holds 0, and such a condition was previously treated as that comparison
// whenever any other existed: listed among the guards, read as a test that
// reads no fact, and omitted from what the levels see the rule read.
TEST_CASE("numeric comparison: a condition without one relation is no comparison")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("(X v A, Y v B, A zelph:lt B) => (X below Y)");
    interactive.process("(P P Z) => (P reflexive Z)");

    const std::string rule = R"((find |(string/find "reflexive" (string ($ :rule))) (zelph/rules)))";
    CHECK(janet_value(collector, interactive, "(length (" + rule + " :guards))") == "0");
    CHECK(janet_value(collector, interactive, "(length (" + rule + " :conditions))") == "1");
}

// The digit alphabet varies according to the substrate, as does every list; the
// &-literals remain unchanged. The cases above run on the decimal substrate.
TEST_CASE("numeric comparison: the conditions compare under every arithmetic module" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        process_lines(interactive, R"(
less l &39
less r &41
equal l &128
equal r &128
greater l &1024
greater r &999
)");
        add_relation_rules(interactive);
        CHECK(relations_of(collector, interactive, "less") == "lt le ne");
        CHECK(relations_of(collector, interactive, "equal") == "le ge eq");
        CHECK(relations_of(collector, interactive, "greater") == "gt ge ne"); });
}

// ---------------------------------------------------------------------------
// The property: on seeded random pairs, the C++ comparator and the six
// conditions order by value, and give the verdict stdlib `cmp` derives wherever
// it derives one -- naturals spanning 1 to 30 digits, equal values expressed in
// one form and in two, leading zeros, and signed integers using the operand
// form (pos zint N). Zero is sampled among them; the line of values above,
// which the selection the packages run holds, pins every form of it. Each
// substrate has its own digits, so each runs the property independently.
// ---------------------------------------------------------------------------

namespace
{
    void check_property(const char* substrate, const int base, const std::uint64_t seed)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        // It is the arithmetic, not the evaluation strategy, that is held
        // to the oracle in this context, and check mode would repeat a
        // classic pass over every arithmetic rule -- roughly double the cost
        // of the case. The cases above run every shape of the condition in
        // check mode.
        interactive.process(".semi-naive on");
        interactive.process(".deductions off");
        interactive.process(std::string(".import ") + substrate);
        interactive.process(".import integer-arithmetic");

        const std::vector<NumericCase>        cases  = random_numeric_cases(seed, 64, base);
        const std::vector<NumericOracleEntry> oracle = stdlib_cmp_oracle(interactive, cases);

        check_every_verdict_occurs(oracle);

        zelph::network::Reasoning* const graph      = interactive.graph();
        const auto                       comparator = [&](const zelph::network::Node a, const zelph::network::Node b)
        { return zelph::network::compare_numbers(*graph, graph->number_vocabulary(), a, b); };
        check_against_stdlib(oracle, comparator);
        check_against_values(oracle, comparator);

        check_conditions_against_values(interactive, oracle);
    }
} // namespace

TEST_CASE("numeric comparison: random pairs are ordered by value and agree with cmp where it answers (decimal-arithmetic)" * doctest::test_suite("slow"))
{
    check_property("decimal-arithmetic", 10, 20261105);
}

TEST_CASE("numeric comparison: random pairs are ordered by value and agree with cmp where it answers (binary-arithmetic)" * doctest::test_suite("slow"))
{
    check_property("binary-arithmetic", 2, 20261106);
}

TEST_CASE("numeric comparison: random pairs are ordered by value and agree with cmp where it answers (binary-nand-arithmetic)" * doctest::test_suite("slow"))
{
    check_property("binary-nand-arithmetic", 2, 20261107);
}
