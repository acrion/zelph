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

#include "network/reasoning.hpp"

#include <algorithm>
#include <filesystem>
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

namespace
{
    // A statement involving multiple objects, which `statement`
    // cannot build.
    Term wide(const Term& subject, const Term& predicate, const std::vector<Term>& objects)
    {
        Term t{"", {subject, predicate}};
        t.parts.insert(t.parts.end(), objects.begin(), objects.end());
        return t;
    }

    // The text of `t`, with the objects of each statement arranged in
    // sorted order: the engine prints the objects of a fact according to
    // its own sequence, and the comparison centres on which statements
    // answer.
    std::string canonical(const Term& t, const bool top = false)
    {
        if (t.parts.empty()) return t.atom;
        std::vector<std::string> objects;
        for (size_t i = 2; i < t.parts.size(); ++i)
            objects.push_back(canonical(t.parts[i]));
        std::ranges::sort(objects);
        std::string inner = canonical(t.parts[0]) + " " + canonical(t.parts[1]);
        for (const std::string& o : objects)
            inner += " " + o;
        return top ? inner : "(" + inner + ")";
    }

    // An answer the engine printed, which is then parsed into a term and
    // written as `canonical` writes it.
    std::string canonical_answer(const std::string& answer)
    {
        std::vector<Term> stack{Term{}};
        std::string       token;
        const auto        flush = [&]
        {
            if (!token.empty()) stack.back().parts.push_back(atom(token));
            token.clear();
        };
        for (const char c : answer)
        {
            if (c == ' ' || c == '(' || c == ')') flush();
            if (c == '(') stack.emplace_back();
            if (c == ')' && stack.size() > 1)
            {
                Term inner = std::move(stack.back());
                stack.pop_back();
                stack.back().parts.push_back(std::move(inner));
            }
            if (c != ' ' && c != '(' && c != ')') token += c;
        }
        flush();
        return canonical(stack.front(), true);
    }

    // The statements `t` states that have one object only at every level:
    // `(j sub k k2) noted yes` states `(j sub k) noted yes` and
    // `(j sub k2) noted yes`.
    std::vector<Term> narrowings(const Term& t)
    {
        if (t.parts.empty()) return {t};
        std::vector<Term> out;
        for (size_t o = 2; o < t.parts.size(); ++o)
            for (const Term& s : narrowings(t.parts[0]))
                for (const Term& p : narrowings(t.parts[1]))
                    for (const Term& n : narrowings(t.parts[o]))
                        out.push_back(statement(s, p, n));
        return out;
    }

    // Whether `t` states `narrower` (logic.md, "Facts with Multiple
    // Objects"): two atoms are identical, and at each level every object
    // referenced by `narrower` is stated by an object in `t`.
    bool states(const Term& t, const Term& narrower)
    {
        if (narrower.parts.empty() || t.parts.empty()) return narrower.parts.empty() && t.parts.empty() && narrower.atom == t.atom;
        if (!states(t.parts[0], narrower.parts[0]) || !states(t.parts[1], narrower.parts[1])) return false;
        for (size_t i = 2; i < narrower.parts.size(); ++i)
        {
            bool found = false;
            for (size_t j = 2; j < t.parts.size() && !found; ++j)
                found = states(t.parts[j], narrower.parts[i]);
            if (!found) return false;
        }
        return true;
    }

    using TermBindings = std::map<std::string, Term>;

    // Each collection of bindings, expanding `bindings`, for which
    // `pattern` matches `t` according to that reading. A variable that
    // remains unbound takes on the node it encounters, while a previously
    // bound one matches every node that states its assigned value, just
    // as a bound variable does in unify_nodes.
    std::vector<TermBindings> subset_matches(const Term& pattern, const Term& t, const TermBindings& bindings)
    {
        if (is_variable(pattern))
        {
            if (const auto it = bindings.find(pattern.atom); it != bindings.end())
                return states(t, it->second) ? std::vector<TermBindings>{bindings} : std::vector<TermBindings>{};
            TermBindings bound = bindings;
            bound.emplace(pattern.atom, t);
            return {bound};
        }
        if (pattern.parts.empty()) return t.parts.empty() && t.atom == pattern.atom ? std::vector<TermBindings>{bindings} : std::vector<TermBindings>{};
        if (t.parts.empty()) return {};

        std::vector<TermBindings> current;
        for (const TermBindings& s : subset_matches(pattern.parts[0], t.parts[0], bindings))
            for (const TermBindings& p : subset_matches(pattern.parts[1], t.parts[1], s))
                current.push_back(p);
        for (size_t i = 2; i < pattern.parts.size(); ++i)
        {
            std::vector<TermBindings> next;
            std::set<std::string>     seen;
            for (const TermBindings& b : current)
                for (size_t j = 2; j < t.parts.size(); ++j)
                    for (const TermBindings& m : subset_matches(pattern.parts[i], t.parts[j], b))
                    {
                        std::string key;
                        for (const auto& [name, value] : m)
                            key += name + "=" + canonical(value) + ";";
                        if (seen.insert(key).second) next.push_back(m);
                    }
            current = std::move(next);
        }
        return current;
    }

    Term instance(const Term& pattern, const TermBindings& bindings)
    {
        if (is_variable(pattern)) return bindings.at(pattern.atom);
        Term out = pattern;
        for (Term& part : out.parts)
            part = instance(part, bindings);
        return out;
    }

    // For every pattern, the results of a query are generated, then the
    // `S hit O` answers once one rule per pattern has run with V bound by
    // `pick has V`, similar to differential_answers. The answers are
    // compared as sets in canonical form: if two facts state the same
    // binding, it appears twice, which is a separate matter.
    std::vector<std::set<std::string>> wide_differential_answers(
        const std::vector<Term>& facts, const std::vector<Abstraction>& patterns, const bool anchors, const bool parallel)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        if (!parallel) interactive.process(".parallel");
        if (!anchors) interactive.process(".anchors off");
        for (const Term& fact : facts)
            interactive.process(canonical(fact, true));

        const auto answers = [&]
        {
            std::set<std::string> out;
            for (const std::string& a : collect_answers(collector))
                out.insert(canonical_answer(a));
            return out;
        };

        std::vector<std::set<std::string>> result;
        for (const Abstraction& a : patterns)
        {
            collector.clear();
            interactive.process(text(a.pattern, true));
            result.push_back(answers());
        }

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
        result.push_back(answers());
        return result;
    }
} // namespace

// The completeness net above, concerning facts that include additional
// objects at a certain level: within a statement's subject, within an
// object, within a composite predicate, two levels deeper, at the middle
// level, and adjacent to a relation that occurs twice. Each pattern arises
// from a narrowing of a fact, which the fact states, and must answer in
// both anchor modes, as a query and with V bound, as a plain matcher using
// the subset reading does. A query asserts the ground statements it holds
// (concepts.md), here narrowings, and a narrowing gives no answer that its
// fact does not already provide. The anchors, the subject-predicate hint,
// and the climb took a statement for the sole node capable of matching it;
// the cases below pin each of these pathways individually.
TEST_CASE("unification: anchors on and off answer every nested pattern alike over facts with more objects")
{
    const std::vector<Term> facts = {
        statement(wide(atom("j"), atom("sub"), {atom("k"), atom("k2")}), atom("noted"), atom("yes")),
        statement(atom("yes"), atom("noted"), wide(atom("a"), atom("sub"), {atom("b"), atom("b2")})),
        statement(statement(atom("j"), wide(atom("a"), atom("r"), {atom("s"), atom("s2")}), atom("k")), atom("noted"), atom("yes")),
        statement(statement(wide(atom("j"), atom("sub"), {atom("k"), atom("k2")}), atom("noted"), atom("yes")), atom("seen"), atom("t")),
        statement(wide(statement(atom("j"), atom("sub"), atom("k")), atom("noted"), {atom("yes"), atom("y2")}), atom("seen"), atom("t")),
        statement(wide(atom("j"), atom("sub"), {atom("k"), atom("k2")}), atom("sub"), atom("m")),
        statement(atom("m"), atom("q"), wide(atom("j"), atom("p"), {atom("k"), atom("k2")})),
        statement(atom("c"), atom("twin"), atom("d")),
    };

    std::vector<Abstraction> patterns;
    // the fact from which each pattern was derived
    // through narrowing
    std::vector<Term> sources;
    for (const Term& fact : facts)
        for (const Term& narrow : narrowings(fact))
        {
            std::vector<Abstraction>       made = abstractions(narrow);
            const std::vector<Abstraction> more = recurrences(narrow);
            made.insert(made.end(), more.begin(), more.end());
            for (Abstraction a : made)
            {
                patterns.push_back(a);
                sources.push_back(fact);
                if (text(a.pattern.parts[2]) == text(narrow.parts[2])) // V is not in the object
                {
                    a.pattern.parts[2] = atom("W");
                    patterns.push_back(a);
                    sources.push_back(fact);
                }
            }
        }

    std::vector<Term> statements;
    for (const Term& fact : facts)
        statements_of(fact, statements);
    std::set<std::string> picked_text;
    std::vector<Term>     picked; // every value of V in `pick has V`
    for (const Abstraction& a : patterns)
        if (picked_text.insert(canonical(a.replaced)).second) picked.push_back(a.replaced);

    std::vector<std::set<std::string>> expected(patterns.size() + 1);
    for (size_t i = 0; i < patterns.size(); ++i)
    {
        for (const Term& s : statements)
            for (const TermBindings& b : subset_matches(patterns[i].pattern, s, {}))
                expected[i].insert(canonical(instance(patterns[i].pattern, b), true));
        for (const Term& v : picked)
            for (const Term& s : statements)
                if (!subset_matches(patterns[i].pattern, s, {{"V", v}}).empty())
                    expected.back().insert(canonical(v) + " hit p" + std::to_string(i));
    }
    for (size_t i = 0; i < patterns.size(); ++i)
    {
        CAPTURE(text(patterns[i].pattern, true));
        REQUIRE_FALSE(subset_matches(patterns[i].pattern, sources[i], {}).empty()); // the matcher itself
    }

    const auto reference = wide_differential_answers(facts, patterns, false, false);
    REQUIRE(reference.size() == expected.size());
    for (size_t i = 0; i < patterns.size(); ++i)
    {
        CAPTURE(text(patterns[i].pattern, true));
        CHECK(reference[i] == expected[i]);
    }
    CHECK(reference.back() == expected.back());

    for (const bool parallel : {false, true})
    {
        CAPTURE(parallel);
        const auto anchored = wide_differential_answers(facts, patterns, true, parallel);
        REQUIRE(anchored.size() == reference.size());
        for (size_t i = 0; i < patterns.size(); ++i)
        {
            CAPTURE(text(patterns[i].pattern, true));
            CHECK(anchored[i] == reference[i]);
        }
        CHECK(anchored.back() == reference.back());
    }
}

// A statement within a pattern is also stated by a fact containing
// additional objects: `(a p b c)` states both `(a p b)` and `(a p c)`. The
// anchors took a concrete statement for the sole node capable of matching
// it, so beside `(a p b c) bad k` neither `(a p b) bad Z` nor `(a p c) bad Z`
// answered, whereas with `.anchors off`, the reference, both did. Whether
// `a p b` exists as a fact of its own does not change that, hence the two
// SUBCASEs.
TEST_CASE("unification: anchors on and off answer a nested statement that a fact with more objects states")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b c\n(a p b c) bad k");
        SUBCASE("the narrower fact is not there") {}
        SUBCASE("the narrower fact is there too")
        {
            interactive.process("a p b");
        }
        for (const char* anchors : {".anchors on", ".anchors off"})
        {
            CAPTURE(anchors);
            interactive.process(anchors);
            collector.clear();
            interactive.process("(a p b) bad Z");
            CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) bad k"});
            collector.clear();
            interactive.process("(a p c) bad Z");
            CHECK(collect_answers(collector) == std::vector<std::string>{"(a p c) bad k"});
        } });
}

// A statement serving as the predicate within a nested statement is
// evaluated through its structure too (unify_nodes): `(a r s s2)` states
// `(a r s)` in that position as well. Two pre-filters treated this
// predicate as the sole node eligible to match: the subject-predicate hint,
// which rejected a candidate whose subject fails to reference `(a r s)`, and
// the climb from a partial anchor, which applies filtering at every level
// based on the predicate. With anchors enabled, neither query yielded an
// answer; with `.anchors off`, both succeeded. Each SUBCASE reaches one of
// these two pre-filters.
TEST_CASE("unification: anchors on and off answer a statement whose predicate is a statement that a fact with more objects states")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string query;
        std::string expected;
        SUBCASE("the predicate of a statement subject")
        {
            interactive.process("(j (a r s s2) k) noted yes");
            query    = "(V (a r s) k) noted yes";
            expected = "(j (a r s) k) noted yes";
        }
        SUBCASE("a predicate further down")
        {
            interactive.process("yes noted (j (a r s s2) k)");
            query    = "X noted (j (a r s) V)";
            expected = "yes noted (j (a r s) k)";
        }
        for (const char* anchors : {".anchors on", ".anchors off"})
        {
            CAPTURE(anchors);
            interactive.process(anchors);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector) == std::vector<std::string>{expected});
        } });
}

// The other two pathways through which an anchor met a concrete statement:
// a variable that was bound to `(a p c)` via a prior condition, which
// unify_nodes compares by its structure, and the statement located beside
// an object that an earlier condition bound, where no partial anchor was
// gathered and the subject was taken first. Both were anchored to the
// statement and failed to match `(a p b c) bad k`, whereas `.anchors off`
// answered.
TEST_CASE("unification: anchors on and off answer a statement a variable is bound to, or one beside a bound object, through a fact with more objects")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b c\n(a p b c) bad k\nk2 says (a p c)\nk is w\nx go y");
        // A ground condition, with an object that is concrete from the start:
        // the identical route as a bound object.
        interactive.process("(x go y, (a p c) bad k) => (x ok y)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S ok O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x ok y"});

        // Thirty statements of the nested shape make the planner evaluate the
        // other condition first, causing V and X to arrive bound.
        interactive.process(R"js(%(for i 0 30 (zelph/fact (zelph/fact (string "m" i) "p" (string "n" i)) "bad" (string "z" i))))js");
        for (const char* anchors : {".anchors on", ".anchors off"})
        {
            CAPTURE(anchors);
            interactive.process(anchors);
            for (const char* query : {"k2 says V, V bad Z", "X is w, (a p c) bad X"})
            {
                CAPTURE(query);
                collector.clear();
                interactive.process(query);
                const auto answers = collect_answers(collector);
                REQUIRE(answers.size() == 1);
                CHECK(answers.front().find("(a p c) bad k") != std::string::npos);
            }
        } });
}

// A file that has been loaded may include a fact involving multiple objects,
// and its header says whether the network that was saved can hold one; after
// loading such a file, the engine reads a statement within a pattern in the
// same way the session does once such a fact is written. The newly
// initialized engine below has written none.
TEST_CASE("unification: after a load, a nested statement is answered through a fact with more objects")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_wider_statement.bin").string();
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "a p b c\n(a p b c) bad k");
        interactive.process(".save " + path);
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path);
    collector.clear();
    interactive.process("(a p c) bad Z");
    CHECK(collect_answers(collector) == std::vector<std::string>{"(a p c) bad k"});
    std::filesystem::remove(path);
}

// The session that saves a network knows whether it holds a fact involving
// multiple objects, and the file explicitly indicates this. Each load
// previously assumed such a fact, causing a statement inside a condition to
// be processed via its atoms within a network that held none: `.explain` of
// the documentation's polynomial identity performed tenfold the work of the
// session that saved it and exceeded its budget (refer to test_explain.cpp).
// A network that may hold such a fact maintains this reading following the
// load, as demonstrated in the case above.
TEST_CASE("unification: a load reads statements through facts with more objects only when the saved network may hold one")
{
    const std::string path             = (std::filesystem::temp_directory_path() / "zelph_test_single_object_facts.bin").string();
    const auto        saved_and_loaded = [&](const std::string& lines)
    {
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, lines);
            interactive.process(".save " + path);
        }
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + path);
        return interactive.graph()->may_hold_wider_statements();
    };

    SUBCASE("a network without one")
    {
        CHECK_FALSE(saved_and_loaded("a p b\n(a p b) bad k"));
    }
    SUBCASE("a network with one")
    {
        CHECK(saved_and_loaded("a p b c\n(a p b c) bad k"));
    }
    std::filesystem::remove(path);
}

// A merge, too, results in a fact that includes additional objects. When a
// statement is assigned the name that another statement holds, the two are
// merged (`.help .name`), and the surviving node holds the objects from
// both: following the merge of `a p b` and `a p c`, a single node states
// `a p b c`, which was never produced by any fact() invocation. The anchors
// took the statement that disappeared during the merge for the sole node
// capable of matching it, hence the question about it answered nothing with
// `.anchors on` and the fact with `.anchors off`. The merge occurring within
// one language and the library's merge of a name pair represent distinct
// routes, with one SUBCASE each.
TEST_CASE("unification: after two statements merge, a nested statement is answered through the fact with more objects")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\na p c\n(a p c) bad k");
        const auto id_of = [&](const std::string& statement)
        {
            collector.clear();
            interactive.process(".node " + statement);
            const std::string marker = "Node ID: ";
            for (const auto& event : collector.events())
                if (const auto pos = event.text.find(marker); pos != std::string::npos)
                    return event.text.substr(pos + marker.size(), event.text.find_first_not_of("0123456789", pos + marker.size()) - pos - marker.size());
            return std::string{};
        };
        const std::string first  = id_of("a p b");
        const std::string second = id_of("a p c");
        REQUIRE_FALSE(first.empty());
        REQUIRE_FALSE(second.empty());
        // the statement that was incorporated into the other one
        std::string vanished;
        SUBCASE("through .name")
        {
            interactive.process(".name " + first + " both");
            interactive.process(".name " + second + " both");
            vanished = "(a p b)";
        }
        SUBCASE("through the library's name pair")
        {
            interactive.process(".name " + first + " both");
            interactive.process(".name " + second + " en other");
            interactive.graph()->set_name("both", "other", "en");
            vanished = "(a p c)";
        }
        collector.clear();
        interactive.process("S bad O");
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"both bad k"});

        for (const char* anchors : {".anchors on", ".anchors off"})
        {
            CAPTURE(anchors);
            interactive.process(anchors);
            collector.clear();
            interactive.process(vanished + " bad Z");
            CHECK(collect_answers(collector) == std::vector<std::string>{vanished + " bad k"});
        } });
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
