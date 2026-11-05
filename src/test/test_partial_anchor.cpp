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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace zelph::test;

namespace zelph::test
{
    // Binary files located in older_networks/*.bin, compiled into this
    // executable via embed_files.cmake.
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

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

// The session that saves a network records the predicates for which it
// might hold a fact involving multiple objects, and the file contains this
// record. Each load previously assumed such a fact, causing a statement
// inside a condition to be processed via its atoms within a network that
// held none: `.explain` of the documentation's polynomial identity
// performed tenfold the work of the session that saved it and exceeded its
// budget (refer to test_explain.cpp). The record specifies predicates, so
// after a load a fact with two objects of `p` leaves every condition over
// `q` to be read by its own node, and `.stat` says how many predicates it
// marks. A network that may hold such a fact maintains the wider reading
// following the load, as demonstrated in the case above.
TEST_CASE("unification: a load reads statements through facts with more objects only for the predicates the saved network marked")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_wider_statement_record.bin").string();
    struct Loaded
    {
        zelph::network::Zelph::WiderStatements record;
        bool                                   p{false};
        bool                                   q{false};
        std::string                            stat; // the line of `.stat` concerning it, if any
    };
    const auto saved_and_loaded = [&](const std::string& lines)
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
        auto* const graph = interactive.graph();
        Loaded      loaded{graph->wider_statements(),
                           graph->may_hold_wider_statements(graph->get_node("p", graph->lang())),
                           graph->may_hold_wider_statements(graph->get_node("q", graph->lang())),
                           {}};
        collector.clear();
        interactive.process(".stat");
        for (const auto& event : collector.events())
            if (const auto at = event.text.find("Facts with multiple objects"); at != std::string::npos)
                loaded.stat = event.text.substr(at, event.text.find('\n', at) - at);
        return loaded;
    };

    SUBCASE("a network without one")
    {
        const Loaded loaded = saved_and_loaded("a p b\n(a p b) bad k\nx q y");
        CHECK_FALSE(loaded.record.unknown);
        CHECK(loaded.record.predicates == 0);
        CHECK_FALSE(loaded.p);
        CHECK_FALSE(loaded.q);
        CHECK(loaded.stat.empty());
    }
    SUBCASE("a network with one")
    {
        const Loaded loaded = saved_and_loaded("a p b c\n(a p b c) bad k\nx q y");
        CHECK_FALSE(loaded.record.unknown);
        CHECK(loaded.record.predicates == 1);
        CHECK(loaded.p);
        CHECK_FALSE(loaded.q);
        CHECK(loaded.stat == "Facts with multiple objects: may exist for 1 predicate");
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

namespace
{
    // The identifier `.node` reports for a node: the path to the id through
    // which a user assigns a name to a statement or a list term.
    std::string node_id(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& node)
    {
        collector.clear();
        interactive.process(".node " + node);
        const std::string marker = "Node ID: ";
        for (const auto& event : collector.events())
            if (const auto pos = event.text.find(marker); pos != std::string::npos)
                return event.text.substr(pos + marker.size(), event.text.find_first_not_of("0123456789", pos + marker.size()) - pos - marker.size());
        return {};
    }

    // Whether the record marks the predicate named `name` within the session's
    // language.
    bool marked(const zelph::console::Interactive& interactive, const std::string& name)
    {
        const auto* const graph = interactive.graph();
        return graph->may_hold_wider_statements(graph->get_node(name, graph->lang()));
    }
} // namespace

// A name merge applies a mark to a predicate solely when it combines the
// objects of two statements: named alike, `a p b` and `a p c` coalesce into
// a single node stating `a p b c`. Each merge involving a statement or a
// set constant on either side marked the session previously, including the
// head merge performed during an RDF list conversion, an atom merged into a
// list term: 300 decimal comparisons took 2.4 s instead of 0.4 s
// afterwards. An atom merged into a statement or a list term transforms
// into that term, and each of its facts is rebuilt over the term using the
// objects it previously held (Zelph::merge_shape), thus no fact gains an
// additional object.
TEST_CASE("unification: a name merge marks a predicate only where it combines the objects of two statements")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto* const graph = interactive.graph();
        interactive.process("hd q z1");
        SUBCASE("an atom merged into a statement")
        {
            interactive.process("a p b");
            interactive.process(".name " + node_id(collector, interactive, "a p b") + " term");
            interactive.process(".name hd term");
            CHECK(graph->wider_statements().predicates == 0);
            collector.clear();
            interactive.process("(a p b) q Z");
            CHECK(collect_answers(collector) == std::vector<std::string>{"term q z1"});
        }
        SUBCASE("an atom merged into a list term")
        {
            interactive.process("cl0 is <u v>");
            interactive.process(".name " + node_id(collector, interactive, "<u v>") + " term");
            interactive.process(".name hd term");
            CHECK(graph->wider_statements().predicates == 0);
            collector.clear();
            interactive.process("<u v> q Z");
            CHECK(collect_answers(collector) == std::vector<std::string>{"term q z1"});
        }
        SUBCASE("two statements")
        {
            process_lines(interactive, "a p b\na p c");
            const std::string first  = node_id(collector, interactive, "a p b");
            const std::string second = node_id(collector, interactive, "a p c");
            interactive.process(".name " + first + " both");
            interactive.process(".name " + second + " both");
            CHECK(graph->wider_statements().predicates == 1);
            CHECK(marked(interactive, "p"));
            CHECK_FALSE(marked(interactive, "q"));
        }
        CHECK_FALSE(graph->wider_statements().unknown); });
}

// What a merge transfers, it places its mark upon the node it relocates
// to: given `a p b c` stated, `.name r p` merges the atom p into r, the
// fact transforms into `a r b c`, and it states `(a r c)` beside
// `(a r b)`. Left with the node that vanished, the mark failed to reach r,
// and `(a p c) bad Z`, queried regarding the merged predicate by its name,
// was read solely by the absent node `a r c` and answered nothing.
TEST_CASE("unification: a merge hands the mark of a predicate on to the node it merges into")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b c\n(a p b c) bad k\nx r y");
        REQUIRE(marked(interactive, "p"));
        REQUIRE_FALSE(marked(interactive, "r"));
        interactive.process(".name r p");
        CHECK(marked(interactive, "p")); // the node r, now named p
        collector.clear();
        interactive.process("(a p c) bad Z");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p c) bad k"}); });
}

// The record answers without its lock while it holds at most four predicates,
// and from its set beyond those (Zelph::may_hold_wider_statements). Six
// predicates that each hold a fact with two objects are all marked, in the
// session that wrote the facts and following a save and a load, and each
// states a statement inside a condition via its fact; a predicate that holds a
// fact with one object stays unmarked. A record that read only the first four
// would ground `(a p5 c)` and `(a p6 c)` to themselves and answer nothing for
// them.
TEST_CASE("unification: the record marks the predicates of facts with two objects beyond the first four as well")
{
    const auto check = [](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        for (int i = 1; i <= 6; ++i)
        {
            const std::string predicate = "p" + std::to_string(i);
            CAPTURE(predicate);
            CHECK(marked(interactive, predicate));
            collector.clear();
            interactive.process("(a " + predicate + " c) bad Z");
            CHECK(collect_answers(collector) == std::vector<std::string>{"(a " + predicate + " c) bad k"});
        }
        CHECK_FALSE(marked(interactive, "q"));
        CHECK(interactive.graph()->wider_statements().predicates == 6);
    };
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_wider_record_six.bin").string();
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        std::string                 lines = "x q y";
        for (int i = 1; i <= 6; ++i)
            lines += "\na p" + std::to_string(i) + " b c\n(a p" + std::to_string(i) + " b c) bad k";
        process_lines(interactive, lines);
        check(collector, interactive);
        interactive.process(".save " + path);
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + path);
        check(collector, interactive);
    }
    std::filesystem::remove(path);
}

// A file created prior to the record's existence does not say which
// predicates might hold a fact with more objects, and its load counts every
// predicate, just as each prior load did; `.stat` confirms this. Saving
// such a network again omits the record, so the next load reads it
// identically. A user reaches this state by loading a file from an older
// engine, such as the one in older_networks/, or any network published
// before the record was established.
TEST_CASE("unification: a file without the record is read as marking every predicate")
{
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("data_collections");
    REQUIRE(network != networks.end());
    const auto path    = std::filesystem::temp_directory_path() / "zelph_test_wider_record_older.bin";
    const auto resaved = std::filesystem::temp_directory_path() / "zelph_test_wider_record_older_resaved.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }
    const auto stat_line = [](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(".stat");
        for (const auto& event : collector.events())
            if (const auto at = event.text.find("Facts with multiple objects"); at != std::string::npos)
                return event.text.substr(at, event.text.find('\n', at) - at);
        return std::string{};
    };
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + path.generic_string());
        CHECK(interactive.graph()->wider_statements().unknown);
        CHECK(stat_line(collector, interactive) == "Facts with multiple objects: may exist for every predicate (not recorded)");
        interactive.process(".save " + resaved.generic_string());
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + resaved.generic_string());
        CHECK(interactive.graph()->wider_statements().unknown);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(resaved);
}

// The record against the reading it superseded, which treated every predicate
// as holding facts with more objects -- the state left after loading a file
// without the record (Zelph::forget_wider_statement_record) -- and against
// the anchor-free reference, `.anchors off`. An atom merged into a statement
// and another merged into a list term each bring a fact for a predicate `q`
// that the term already holds a fact for, along with a statement regarding
// each of those two facts: had a merge combined the two into a single fact
// encompassing both objects and left `q` unmarked, the record would read the
// statements about them via the single-object facts that no longer exist, and
// yield fewer answers than the reading that counts every predicate. Neither
// merge combines them, and neither marks a predicate. Facts involving two
// objects of `s` come next, one also nested within a statement whose own
// predicate `t` holds none, and a rule whose second condition, `X s n2`, is
// bound throughout once the first condition, seeded with `m2 trig e` by
// `.run-delta`, has bound X: a probe ignoring the mark of `s` would seek the
// triple `m2 s n2` exclusively and overlook `m2 s n2 o2`, which states it.
// The forgotten reading runs the same probe, so such a probe would likewise
// miss `m2 ok e` within it; the anchor-free reference and the expected answer
// confirm this. Seeded, because a `.run` starts with a classic pass, which
// evaluates the rule with X unbound and fires it regardless; and not over
// `m s n o`, since the statements derived about it assert `m s n` on its own
// (a statement asserts the facts nested within it), which the probe detects.
// Every query and every derivation yields identical results across all three
// readings, even when derivations are made under the forgotten one, and after
// a save and a load.
TEST_CASE("unification: the record answers as the reading that counts every predicate after merges, facts with two objects and a load")
{
    const std::vector<std::string> queries = {"(a p b) q Z", "X q z1", "(a p B) q Z", "<u v> q Z", "(u cons Y) q Z", "((a p b) q z1) r W", "((a p b) q z2) r W", "((a p b) q Z) r W", "X r w1", "X r w2", "(<u v> q z3) r W", "(<u v> q Z) r W", "(m s n) bad Z", "(m s o) bad Z", "m s X", "(m s X) bad Z", "S bad k", "W seen V", "(m s n) seen K", "((m s n) t u) bad Z", "((m s X) t U) bad Z", "m2 ok Y"};
    const auto                     answers = [&queries](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        std::vector<std::vector<std::string>> all;
        for (const std::string& query : queries)
        {
            collector.clear();
            interactive.process(query);
            auto found = collect_answers(collector);
            std::sort(found.begin(), found.end());
            all.push_back(found);
        }
        return all;
    };
    const auto build = [](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        process_lines(interactive, "(X q Y) => (X seen Y)\n((A s B) bad K) => ((A s B) seen K)\n(X trig Y, X s n2) => (X ok Y)\n"
                                   "hd q z1\n(hd q z1) r w1\na p b\n(a p b) q z2\n((a p b) q z2) r w2\n"
                                   "hd2 q z3\n(hd2 q z3) r w3\ncl0 is <u v>\n<u v> q z4\n(<u v> q z4) r w4\n"
                                   "m s n o\n(m s n o) bad k\n((m s n o) t u) bad k\nm2 s n2 o2");
        interactive.process(".name " + node_id(collector, interactive, "a p b") + " term1");
        interactive.process(".name hd term1");
        interactive.process(".name " + node_id(collector, interactive, "<u v>") + " term2");
        interactive.process(".name hd2 term2");
        process_lines(interactive, ".semi-naive on\n.auto-run\nm2 trig e\n.run-delta"); // .auto-run serves as a toggle: off
    };
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_wider_record_differential.bin").string();

    std::vector<std::vector<std::string>> recorded;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        build(collector, interactive);
        REQUIRE(interactive.graph()->wider_statements().predicates == 1); // s
        recorded = answers(collector, interactive);
        interactive.process(".save " + path);
        interactive.graph()->forget_wider_statement_record();
        CHECK(answers(collector, interactive) == recorded);
    }
    for (std::size_t i = 0; i < queries.size(); ++i)
    {
        CAPTURE(queries[i]);
        CHECK_FALSE(recorded[i].empty()); // each query pertains to something that holds
    }
    // Both facts regarding q remain separate following each merge, and so do
    // the statements concerning them; the rule over `X s n2` was fired.
    const auto answered = [&](const std::string& query)
    { return recorded[static_cast<std::size_t>(std::find(queries.begin(), queries.end(), query) - queries.begin())]; };
    CHECK(answered("(a p b) q Z").size() == 2);
    CHECK(answered("((a p b) q Z) r W").size() == 2);
    CHECK(answered("(<u v> q Z) r W").size() == 2);
    CHECK(answered("m2 ok Y") == std::vector<std::string>{"m2 ok e"});
    {
        // The derivations, made under the reading that counts every predicate.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.graph()->forget_wider_statement_record();
        build(collector, interactive);
        CHECK(answers(collector, interactive) == recorded);
    }
    {
        // The anchor-free reference, which neither a probe nor an anchor
        // reaches.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".anchors off");
        build(collector, interactive);
        CHECK(answers(collector, interactive) == recorded);
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + path);
        CHECK(interactive.graph()->wider_statements().predicates == 1);
        CHECK(answers(collector, interactive) == recorded);
        interactive.graph()->forget_wider_statement_record();
        CHECK(answers(collector, interactive) == recorded);
    }
    std::filesystem::remove(path);
}

// Once a predicate is marked, whether a statement may be matched by a fact
// with more objects is determined through a walk across the statements
// inside it (may_match_wider_statement), entering each of them exactly
// once. A list of 5000 items corresponds to 5000 statements: more than the
// walk can distinguish within its small buffer, and more than it keeps
// between successive walks. Inquired about twice, and about a short list
// in between, the conditions answer as if under the reading that counts
// every predicate, which never proceeds beyond the first statement. Slow:
// two sessions construct the list.
TEST_CASE("unification: a condition over a list of many items answers alike once a predicate is marked" * doctest::test_suite("slow"))
{
    std::string items;
    for (int i = 0; i < 5000; ++i)
        items += (i == 0 ? "i" : " i") + std::to_string(i);
    const std::string long_list = "<" + items + ">";
    const auto        answers   = [&long_list](const bool forgotten)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        if (forgotten) interactive.graph()->forget_wider_statement_record();
        process_lines(interactive, ".deductions off\nzz p a b\n" + long_list + " q k\n<x y> q k2");
        std::vector<std::vector<std::string>> found;
        for (const std::string& query : {long_list + " q Z", std::string("<x y> q Z"), long_list + " q Z"})
        {
            collector.clear();
            interactive.process(query);
            found.push_back(collect_answers(collector));
        }
        return found;
    };
    const auto recorded = answers(false);
    REQUIRE(recorded.size() == 3);
    CHECK(recorded[0] == std::vector<std::string>{long_list + " q k"});
    CHECK(recorded[1] == std::vector<std::string>{"<x y> q k2"});
    CHECK(recorded[2] == recorded[0]);
    CHECK(answers(true) == recorded);
}

// The probe and the record against the readings they superseded, within
// the rule programs whose cost they decide: the stdlib's comparisons,
// whose digit tests are bound throughout, and the polynomial stack, whose
// conditions hold statements at every level. Each program responds
// identically with the record, with the record forgotten -- every
// predicate counted as holding facts with more objects, just as before the
// record's existence, which likewise leaves no condition for the probe --
// and devoid of anchors, the anchor-free reference. Slow: nine imports of
// the arithmetic stack.
TEST_CASE("unification: the stdlib's comparisons and the polynomial stack answer alike with the record, without it, and without anchors" * doctest::test_suite("slow"))
{
    const auto answers = [](const std::string& reading, const std::string& setup, const std::vector<std::string>& queries)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        if (reading == "forgotten") interactive.graph()->forget_wider_statement_record();
        if (reading == "no anchors") interactive.process(".anchors off");
        process_lines(interactive, ".deductions off\n" + setup);
        std::vector<std::vector<std::string>> all;
        for (const std::string& query : queries)
        {
            collector.clear();
            interactive.process(query);
            auto found = collect_answers(collector);
            std::sort(found.begin(), found.end());
            all.push_back(found);
        }
        return all;
    };
    const auto check = [&](const std::string& setup, const std::vector<std::string>& queries)
    {
        const auto recorded = answers("recorded", setup, queries);
        for (const auto& found : recorded)
            CHECK_FALSE(found.empty());
        CHECK(answers("forgotten", setup, queries) == recorded);
        CHECK(answers("no anchors", setup, queries) == recorded);
    };
    const std::string              pairs    = "c0 is (&2408971 cmp &2334539)\nc1 is (&2334539 cmp &2408971)\nc2 is (&2408971 cmp &2408971)\n"
                                              "c3 is (&19 cmp &190)\nc4 is (&190 cmp &109)\nc5 is (&0 cmp &1)";
    const std::vector<std::string> verdicts = {"A < B", "A > B", "A == B", "(A cmp B) = V"};
    SUBCASE("decimal comparisons") { check(".import decimal-arithmetic\n" + pairs, verdicts); }
    SUBCASE("binary comparisons") { check(".import binary-arithmetic\n" + pairs, verdicts); }
    SUBCASE("polynomial identities")
    {
        check(".import math\n<x y> ~ polyring\n(x + y) ≡ (y + x)\n$( (x+1)*(x-1) ) ≡ $( x^2 - 1 )\n$( (x+y)^2 ) ≡ $( x^2 + y^2 )",
              {"(A ≡ B) = R"});
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

namespace
{
    // One counter from the `.prof` dump (counter mode, `.log -1`): `section`
    // designates the line, `key` identifies the counter within it.
    std::uint64_t prof_counter(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& section, const std::string& key)
    {
        collector.clear();
        interactive.process(".prof");
        std::string prof;
        for (const auto& event : collector.events())
            prof += event.text + "\n";
        const size_t start = prof.find(section);
        REQUIRE(start != std::string::npos);
        const size_t at = prof.find(key + "=", start);
        REQUIRE(at != std::string::npos);
        return std::stoull(prof.substr(at + key.size() + 1));
    }

    // 300 facts that hold `node` as their subject, or as their object: what
    // fills the adjacency of a digit, which every list cell holding the
    // digit names.
    void fill_adjacency(const zelph::console::Interactive& interactive, const std::string& node, const bool as_subject)
    {
        interactive.process(as_subject ? R"js(%(for i 0 300 (zelph/fact ")js" + node + R"js(" "inlist" (string "c" i))))js"
                                       : R"js(%(for i 0 300 (zelph/fact (string "c" i) "flag" ")js" + node + R"js(")))js");
    }
} // namespace

// A condition in which the relation, subject, and objects are all bound was
// read from the adjacency of its subject, which every fact naming the
// subject fills: `A isdigit true` in the stdlib's comparison rules
// (common-arithmetic.zph), with A bound to a digit, walked every list cell
// holding that digit, so that each comparison expanded in tandem with the
// numbers within the graph -- 4000 seven-digit comparisons took 20 s
// instead of 3 s decimal, and 822 s instead of 10 s binary. It is answered
// by a single exact probe, the candidate it identifies tried just like any
// other. Whichever condition the planner takes first, the other is bound
// throughout upon arrival, and the 300 facts naming `d` are never walked.
TEST_CASE("unification: a condition bound throughout is answered by one exact probe")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, ".deductions off\nd isdigit true\nd trig e");
        fill_adjacency(interactive, "d", true);
        interactive.process(".log -1");
        interactive.process(".prof reset");
        collector.clear();
        interactive.process("X trig e, X isdigit true");
        const auto answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front().find("d trig e") != std::string::npos);
        CHECK(answers.front().find("d isdigit true") != std::string::npos);
        CHECK(prof_counter(collector, interactive, "anchors:", "exact_probes") == 1);
        CHECK(prof_counter(collector, interactive, "anchors:", "walked") < 300);

        // The probe is an anchor-based lookup, and `.anchors off`, the
        // anchor-free reference, excludes it alongside the others.
        interactive.process(".anchors off");
        interactive.process(".prof reset");
        collector.clear();
        interactive.process("X trig e, X isdigit true");
        const auto reference = collect_answers(collector);
        REQUIRE(reference.size() == 1);
        CHECK(reference.front().find("d isdigit true") != std::string::npos);
        CHECK(prof_counter(collector, interactive, "anchors:", "exact_probes") == 0); });
}

// Where the probe cannot answer the condition, as a fact with two objects of
// its relation might state it, the condition is read at the smaller of its
// two bound ends: every fact it matches holds the subject as its subject and
// the object among its objects, so either adjacency yields all of them. Read
// at the subject alone, the 300 facts naming `d` were walked for each test.
// The single fact that answers the digit test is such a fact,
// `d isdigit true maybe`: a probe for the triple `d isdigit true`, executed
// although its relation holds a fact with two objects, finds nothing, and the
// rule does not fire. The rule is fired by its first condition, seeded with
// `d trig e` via `.run-delta`, so X is bound before the digit test, as the
// stdlib's comparison rules are evaluated. A `.run` starts with a classic
// pass, and `.semi-naive check` adds classic passes, both of which evaluate
// the rule with X unbound: one condition then reads the adjacency of `d` or
// of `true` with a single end bound, regardless of the rule for two bound
// ends.
TEST_CASE("unification: a condition bound at both ends that no probe answers is read at the smaller adjacency")
{
    const auto session = [](const bool at_subject)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".semi-naive on\n.deductions off\n(X trig Y, X isdigit true) => (X ok Y)\nd isdigit true maybe");
        fill_adjacency(interactive, at_subject ? "d" : "true", at_subject);
        interactive.process(".auto-run"); // a toggle: off
        interactive.process(".log -1");
        interactive.process(".prof reset");
        interactive.process("d trig e");
        interactive.process(".run-delta");
        const std::uint64_t walked = prof_counter(collector, interactive, "anchors:", "walked");
        interactive.process(".log 0");
        collector.clear();
        interactive.process("d ok Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"d ok e"});
        return walked;
    };
    SUBCASE("the subject's adjacency is the large one") { CHECK(session(true) < 300); }
    SUBCASE("the object's adjacency is the large one") { CHECK(session(false) < 300); }
}
