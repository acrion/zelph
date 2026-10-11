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
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Partial-pattern anchoring (unification.cpp): a condition whose structured
// subject is only PARTIALLY bound -- the SC-congruence shape
// ((U op v) mark (U op v)) with bound v inside the pattern -- must not scan
// the whole relation; the candidate set is climbed from the bound inner
// node. Three pins:
//  1. completeness: anchored evaluation (both parallelism modes) finds the
//     same matches as the anchor-free naive reference (.anchors off),
//     additionally guarded by `.semi-naive check`;
//  2. the anchor path is actually taken in optimized mode (log marker --
//     red without the feature);
//  3. a hub anchor exceeding the work budget falls back to the full scan
//     and stays complete (no marker, same matches).
// ---------------------------------------------------------------------------

namespace
{
    template <typename Interactive>
    void setup_mark_workload(Interactive& interactive)
    {
        // 40 facts involving v, plus 40 distractors that share the mark
        // relation but not v: a complete result has exactly 40 matches, an
        // anchored candidate set never visits the distractors.
        interactive.process(R"js(%(each i (range 40) (def p (zelph/fact (string "u" i) "op" "v")) (zelph/fact p "mark" p)))js");
        interactive.process(R"js(%(each i (range 40) (def q (zelph/fact (string "w" i) "op" (string "z" i))) (zelph/fact q "mark" q)))js");
        process_lines(interactive, R"(
(V go V) => (V trig t)
(V trig Q,
 (U op V) mark (U op V))
=> ((U op V) found Q)
)");
    }
} // namespace

TEST_CASE("unification: partial-pattern anchor finds all matches of a partially bound pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        setup_mark_workload(interactive);
        interactive.process("v go v");
        collector.clear();
        interactive.process("_A found _B");
        CHECK(collect_answers(collector).size() == 40);
        CHECK(answers_contain(collector, "(u0 op v) found t"));
        CHECK(answers_contain(collector, "(u39 op v) found t")); });
}

TEST_CASE("unification: partial-pattern anchor is taken in optimized mode")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        setup_mark_workload(interactive);
        interactive.process(".log 1");
        collector.clear();
        interactive.process("v go v");
        CHECK(any_event_contains(collector, "partial-anchor:")); });
}

TEST_CASE("unification: hub anchor exceeding the work budget falls back to the full scan")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        // Blow up the anchor's adjacency well past the work budget
        // (max(1024, 16 * extent) with extent 3): 1200 unrelated facts on
        // the hub h. The climb must abort and the full scan of the tiny
        // mark2 relation must still deliver all 3 matches.
        interactive.process(R"js(%(each i (range 1200) (zelph/fact (string "x" i) "touch" "h")))js");
        interactive.process(R"js(%(each i (range 3) (def p (zelph/fact (string "k" i) "op2" "h")) (zelph/fact p "mark2" p)))js");
        process_lines(interactive, R"(
(V go2 V) => (V trig2 t)
(V trig2 Q,
 (U op2 V) mark2 (U op2 V))
=> ((U op2 V) found2 Q)
)");
        interactive.process(".log 1");
        interactive.process("h go2 h");
        collector.clear();
        interactive.process("_A found2 _B");
        CHECK(collect_answers(collector).size() == 3);
        CHECK(answers_contain(collector, "(k0 op2 h) found2 t")); });
}

TEST_CASE("unification: partial-pattern anchor engages in single-core mode")
{
    // Red before the use_anchors() decoupling: the anchor-construction
    // block sat inside a use_parallel() guard, so classic evaluation fell
    // back to full-relation scans (the >20 min single-core det gap).
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        setup_mark_workload(interactive);
        interactive.process(".log 1");
        collector.clear();
        interactive.process("v go v");
        CHECK(any_event_contains(collector, "partial-anchor:")); });
}

TEST_CASE("unification: .anchors off restores the anchor-free naive reference")
{
    // THE independent completeness net for anchoring: with anchors off,
    // every condition scans the full relation extent, and the results must
    // be identical. (The former reference -- plain single-core mode -- now
    // anchors too, by design.)
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        interactive.process(".anchors off");
        setup_mark_workload(interactive);
        interactive.process(".log 1");
        collector.clear();
        interactive.process("v go v");
        CHECK_FALSE(any_event_contains(collector, "partial-anchor:"));
        interactive.process(".log 0");
        collector.clear();
        interactive.process("_A found _B");
        CHECK(collect_answers(collector).size() == 40);
        CHECK(answers_contain(collector, "(u0 op v) found t"));
        CHECK(answers_contain(collector, "(u39 op v) found t")); });
}

namespace
{
    // A term of the differential test outlined below: an atom, or a
    // statement comprising subject, predicate, and object.
    struct Term
    {
        std::string       atom;
        std::vector<Term> parts;
    };

    Term atom(const std::string& name)
    {
        return {name, {}};
    }

    Term statement(const Term& subject, const Term& predicate, const Term& object)
    {
        return {"", {subject, predicate, object}};
    }

    std::string text(const Term& t, const bool top = false)
    {
        if (t.parts.empty()) return t.atom;
        const std::string inner = text(t.parts[0]) + " " + text(t.parts[1]) + " " + text(t.parts[2]);
        return top ? inner : "(" + inner + ")";
    }

    struct Abstraction
    {
        Term pattern;
        Term replaced; // what the variable V represents in the source statement
    };

    // Each constituent subterm of `t`, one after another, substituted
    // with V, at every level of nesting.
    std::vector<Abstraction> abstractions(const Term& t)
    {
        std::vector<Abstraction> out;
        for (size_t i = 0; i < t.parts.size(); ++i)
        {
            Term pattern     = t;
            pattern.parts[i] = atom("V");
            out.push_back({pattern, t.parts[i]});
            for (const Abstraction& inner : abstractions(t.parts[i]))
            {
                Term deeper     = t;
                deeper.parts[i] = inner.pattern;
                out.push_back({deeper, inner.replaced});
            }
        }
        return out;
    }

    // `t` with each instance of the atom `name` replaced by `by`.
    Term replace_atom(const Term& t, const std::string& name, const Term& by)
    {
        if (t.parts.empty()) return t.atom == name ? by : t;
        Term out = t;
        for (Term& part : out.parts)
            part = replace_atom(part, name, by);
        return out;
    }

    void count_atoms(const Term& t, std::map<std::string, int>& counts)
    {
        if (t.parts.empty()) ++counts[t.atom];
        for (const Term& part : t.parts)
            count_atoms(part, counts);
    }

    // Every atom appearing more than once in `t`, with all its instances
    // substituted by V: `(j sub k) sub m` results in `(j V k) V m`. A
    // second variant also opens every remaining atom, yielding
    // `(A V B) V C`, such that only the recurrence of V restricts the
    // match.
    std::vector<Abstraction> recurrences(const Term& t)
    {
        std::map<std::string, int> counts;
        count_atoms(t, counts);
        std::vector<Abstraction> out;
        for (const auto& [name, count] : counts)
        {
            if (count < 2) continue;
            Term pattern = replace_atom(t, name, atom("V"));
            out.push_back({pattern, atom(name)});
            char next = 'A';
            for (const auto& [other, _] : counts)
                if (other != name)
                    pattern = replace_atom(pattern, other, atom(std::string(1, next++)));
            out.push_back({pattern, atom(name)});
        }
        return out;
    }

    bool is_variable(const Term& t)
    {
        return t.parts.empty() && t.atom.size() == 1 && t.atom[0] >= 'A' && t.atom[0] <= 'Z';
    }

    // A plain matcher against which the engine's answers are evaluated: a
    // variable binds to a subterm, and every subsequent appearance of it
    // must be that same subterm.
    bool matches(const Term& pattern, const Term& t, std::map<std::string, std::string>& bindings)
    {
        if (is_variable(pattern))
        {
            const auto [it, inserted] = bindings.emplace(pattern.atom, text(t));
            return inserted || it->second == text(t);
        }
        if (pattern.parts.size() != t.parts.size()) return false;
        if (pattern.parts.empty()) return pattern.atom == t.atom;
        for (size_t i = 0; i < t.parts.size(); ++i)
            if (!matches(pattern.parts[i], t.parts[i], bindings)) return false;
        return true;
    }

    // `t` along with every statement contained within it.
    void statements_of(const Term& t, std::vector<Term>& out)
    {
        if (t.parts.empty()) return;
        out.push_back(t);
        for (const Term& part : t.parts)
            statements_of(part, out);
    }

    // The sorted answers to each pattern as a query, followed by the sorted
    // `S hit O` answers after one rule has executed per pattern, with V
    // bound by the rule's condition `pick has V`.
    std::vector<std::vector<std::string>> differential_answers(
        const std::vector<Term>& facts, const std::vector<Abstraction>& patterns, const bool anchors, const bool parallel)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        if (!parallel) interactive.process(".parallel");
        if (!anchors) interactive.process(".anchors off");
        for (const Term& fact : facts)
            interactive.process(text(fact, true));

        std::vector<std::vector<std::string>> result;
        for (const Abstraction& a : patterns)
        {
            collector.clear();
            interactive.process(text(a.pattern, true));
            result.push_back(collect_answers(collector));
            std::ranges::sort(result.back());
        }

        // `go has x` derives `pick has x` during the first iteration,
        // and the subsequent iteration seeds each rule's `pick has V`
        // with it: the pattern is matched with V bound.
        interactive.process(".auto-run"); // a toggle: off
        interactive.process("(go has V) => (pick has V)");
        for (size_t i = 0; i < patterns.size(); ++i)
            interactive.process("(pick has V, " + text(patterns[i].pattern, true) + ") => (V hit p" + std::to_string(i) + ")");
        for (const Abstraction& a : patterns)
            interactive.process("go has " + text(a.replaced));
        interactive.process(".auto-run"); // on again
        interactive.process(".run");

        collector.clear();
        interactive.process("S hit O");
        result.push_back(collect_answers(collector));
        std::ranges::sort(result.back());
        return result;
    }
} // namespace

TEST_CASE("unification: anchors on and off answer every nested pattern alike")
{
    // The completeness net for the anchor lookups and the subject-predicate
    // hint: every pattern corresponds to a statement from the graph in
    // which a single subterm is substituted by V, located in the subject,
    // predicate, or object, at depth 1 and 2, within both a plain and a
    // composite predicate; and additionally, with the top-level object open
    // as W, leaving the subject's inner nodes as the only anchors. Each is
    // queried (V unbound) and then matched as a rule condition with V
    // bound, under both anchor modes -- on and off -- and must yield
    // identical results. A statement subject featuring a variable predicate
    // failed to match anything in either mode since commit 5551eb2, and a
    // composite predicate holding a variable filtered the anchor climb.
    //
    // A flaw common to both modes satisfies the comparison, thus the
    // reference must deliver precisely the same response as a plain matcher
    // does when processing the statements and their nested parts. The
    // patterns made from an atom that appears repeatedly within a
    // statement, `(j V k) V m` and `(A V B) V C`, demand that V represent
    // the identical relation in both places: `(j p k) q m` and
    // `m q (j p k)` match them except for this detail, and they were
    // answered as `(j p k) p m` and `m p (j p k)`.
    const std::vector<Term> facts = {
        statement(statement(atom("j"), atom("sub"), atom("k")), atom("noted"), atom("yes")),
        statement(atom("yes"), atom("noted"), statement(atom("a"), atom("sub"), atom("b"))),
        statement(statement(atom("j"), statement(atom("a"), atom("r"), atom("s")), atom("k")), atom("noted"), atom("yes")),
        statement(statement(statement(atom("j"), atom("sub"), atom("k")), atom("noted"), atom("yes")), atom("seen"), atom("t")),
        statement(statement(statement(atom("j"), statement(atom("a"), atom("r"), atom("s")), atom("k")), atom("noted"), atom("yes")), atom("seen"), atom("t")),
        statement(atom("c"), atom("twin"), atom("d")),
        statement(statement(atom("j"), atom("sub"), atom("k")), atom("sub"), atom("m")),
        statement(statement(atom("j"), atom("p"), atom("k")), atom("q"), atom("m")),
        statement(atom("m"), atom("sub"), statement(atom("j"), atom("sub"), atom("k"))),
        statement(atom("m"), atom("q"), statement(atom("j"), atom("p"), atom("k"))),
    };

    std::vector<Abstraction> patterns;
    std::vector<std::string> sources; // the statement each pattern was made from
    for (const Term& fact : facts)
    {
        std::vector<Abstraction>       made = abstractions(fact);
        const std::vector<Abstraction> more = recurrences(fact);
        made.insert(made.end(), more.begin(), more.end());
        for (Abstraction a : made)
        {
            patterns.push_back(a);
            sources.push_back(text(fact, true));
            if (text(a.pattern.parts[2]) == text(fact.parts[2])) // V is not in the object
            {
                a.pattern.parts[2] = atom("W");
                patterns.push_back(a);
                sources.push_back(text(fact, true));
            }
        }
    }

    std::vector<Term> statements;
    for (const Term& fact : facts)
        statements_of(fact, statements);
    std::set<std::string> picked; // every value of V in `pick has V`
    for (const Abstraction& a : patterns)
        picked.insert(text(a.replaced));

    const auto reference = differential_answers(facts, patterns, false, false);
    REQUIRE(reference.size() == patterns.size() + 1);
    std::set<std::string> hits;
    for (size_t i = 0; i < patterns.size(); ++i)
    {
        CAPTURE(text(patterns[i].pattern, true));
        std::set<std::string> answers;
        for (const Term& s : statements)
        {
            std::map<std::string, std::string> bindings;
            if (!matches(patterns[i].pattern, s, bindings)) continue;
            answers.insert(text(s, true));
            if (picked.contains(bindings["V"]))
                hits.insert(bindings["V"] + " hit p" + std::to_string(i));
        }
        REQUIRE(answers.contains(sources[i])); // the matcher itself
        CHECK(reference[i] == std::vector<std::string>(answers.begin(), answers.end()));
    }
    CHECK(reference.back() == std::vector<std::string>(hits.begin(), hits.end()));

    for (const bool parallel : {false, true})
    {
        CAPTURE(parallel);
        const auto anchored = differential_answers(facts, patterns, true, parallel);
        REQUIRE(anchored.size() == reference.size());
        for (size_t i = 0; i < patterns.size(); ++i)
        {
            CAPTURE(text(patterns[i].pattern, true));
            CHECK(anchored[i] == reference[i]);
        }
        CHECK(anchored.back() == reference.back());
    }
}

TEST_CASE("unification: .anchors off switches off the subject-predicate hint as well")
{
    // The hint skips a candidate whose statement subject does not point to
    // the predicate of the pattern's subject, before the start of
    // extraction. It functions as a candidate pre-filter akin to anchors,
    // and a reference sharing a filter cannot catch that filter's defects:
    // the hint once rejected every candidate of a variable inner predicate,
    // whether anchors were enabled or disabled. When anchors are disabled,
    // `(a twin b) noted yes` proceeds to extraction and then fails; when
    // anchors are enabled, the hint skips it.
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        interactive.process("(j sub k) noted yes");
        interactive.process("(a twin b) noted yes");

        for (const bool anchors : {false, true})
        {
            CAPTURE(anchors);
            interactive.process(anchors ? ".anchors on" : ".anchors off");
            interactive.process(".log 2");
            collector.clear();
            interactive.process("(X sub Y) noted O");
            interactive.process(".log 0");
            CHECK(any_event_contains(collector, "FactSubj=a twin b") == !anchors);
            CHECK(answers_contain(collector, "(j sub k) noted yes"));
        } });
}

TEST_CASE("unification: a composite predicate holding a variable does not filter the anchor climb")
{
    // (X (A r s) k) noted O anchors to k and climbs to the statements that
    // hold it. The climb restricted that level using the pattern's own
    // (A r s), which no data statement references, thus leaving only the
    // query's own pattern as the sole candidate, and
    // `(j (a r s) k) noted yes` was never examined. A predicate variable
    // serves as no filter; a composite predicate containing one likewise
    // acts as no filter.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(j (a r s) k) noted yes");
        interactive.process("(j sub k) noted yes");

        interactive.process(".log 1");
        collector.clear();
        interactive.process("(X (A r s) k) noted O");
        interactive.process(".log 0");
        CHECK(any_event_contains(collector, "partial-anchor:"));
        CHECK(collect_answers(collector) == std::vector<std::string>{"(j (a r s) k) noted yes"});

        interactive.process(".anchors off");
        collector.clear();
        interactive.process("(X (A r s) k) noted O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(j (a r s) k) noted yes"}); });
}

TEST_CASE("unification: .anchors command reports and switches modes")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        collector.clear();
        interactive.process(".anchors");
        CHECK(any_output_contains(collector, "Anchor-based lookups: on"));

        collector.clear();
        interactive.process(".anchors off");
        CHECK(any_output_contains(collector, "Anchor-based lookups: off"));

        collector.clear();
        interactive.process(".anchors on");
        CHECK(any_output_contains(collector, "Anchor-based lookups: on"));

        CHECK_THROWS_AS(interactive.process(".anchors banana"), std::runtime_error); });
}

TEST_CASE("unification: subject/object-driven anchors engage in single-core mode")
{
    // Red when increment_fact_index's anchor branch is gated on
    // use_parallel() instead of use_anchors(): the remaining condition
    // (B bar C) of the seeded rule has its subject bound and must take
    // the subject-driven anchor, logged as optimized_snapshot=YES.
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        process_lines(interactive, R"(
m1 foo n1
n1 bar p1
(A foo B, B bar C) => (A chain C)
)");
        interactive.process(".log 2");
        collector.clear();
        interactive.process("m2 foo n1");
        CHECK(any_event_contains(collector, "optimized_snapshot=YES")); });
}
