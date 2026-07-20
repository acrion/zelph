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

#pragma once

#include <doctest/doctest.h>

#include "interactive.hpp"
#include "io/output.hpp"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

namespace zelph::test
{
    // Collapse runs of whitespace to a single space, trim leading/trailing.
    inline std::string normalize(const std::string& s)
    {
        std::string result;
        bool        in_space = true;
        for (char c : s)
        {
            if (c == ' ' || c == '\t')
            {
                if (!in_space)
                {
                    result += ' ';
                    in_space = true;
                }
            }
            else
            {
                result += c;
                in_space = false;
            }
        }
        if (!result.empty() && result.back() == ' ')
            result.pop_back();
        return result;
    }
    // ------------------------------------------------------------------
    // Self-fact sugar equivalence
    //
    // The display renders eligible self-facts (subject == object) as
    // ":pred subject". Since numerals and digits are hash-consed, this
    // includes facts like (&1 + &1) or (1 nand 1), so verbose expected
    // strings alone would no longer match. sugar_canonical() maps BOTH
    // sides of a comparison onto a common form: verbose "S P S" triples
    // are contracted to ":P S" (using the same predicate-name gate as the
    // display, so e.g. "(&9 * &9)" stays verbose on both sides), and
    // whitespace adjacent to ()/{} delimiters is dropped (identifier
    // markers around ":pred" widen to spaces on unmark). Every comparison
    // helper accepts the plain normalized match OR the canonical match.
    // Negative checks thereby become STRICTER, not weaker: a CHECK_FALSE
    // on "x racewin x" also rejects ":racewin x". Snippets may be
    // unbalanced prefixes ("((&2 testprime &2) = prime"); the parser
    // tolerates missing closers and stray ')' / '}'.
    // ------------------------------------------------------------------
    namespace sugar_detail
    {
        struct Token
        {
            std::string text;
            bool        spaced; // preceded by whitespace in the source
        };

        // Mirror of the display gate in node_to_string.cpp.
        inline bool sugarable_predicate(const std::string& t)
        {
            if (t.empty()) return false;
            if (t.size() == 1 && t[0] >= 'A' && t[0] <= 'Z') return false; // variable
            if (t[0] == '_') return false;                                 // variable
            for (const char ch : t)
            {
                const unsigned char c = static_cast<unsigned char>(ch);
                if (c <= ' ') return false;
                switch (c)
                {
                case '<':
                case '>':
                case '(':
                case ')':
                case '{':
                case '}':
                case '*':
                case ',':
                case '"':
                    return false;
                default:
                    break;
                }
                if (c == 0xC2) return false; // '¬', '«', '»'
            }
            return true;
        }

        // Contract "S P S" token triples to ":P S" until fixpoint. The
        // operand may itself be a (recursively contracted) group token.
        inline void contract(std::vector<Token>& toks)
        {
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (size_t i = 0; i + 2 < toks.size(); ++i)
                {
                    if (!toks[i].text.empty()
                        && toks[i].text == toks[i + 2].text
                        && sugarable_predicate(toks[i + 1].text))
                    {
                        const Token pred{":" + toks[i + 1].text, toks[i].spaced};
                        const Token operand{toks[i].text, true};
                        toks.erase(toks.begin() + static_cast<std::ptrdiff_t>(i),
                                   toks.begin() + static_cast<std::ptrdiff_t>(i + 3));
                        toks.insert(toks.begin() + static_cast<std::ptrdiff_t>(i), {pred, operand});
                        changed = true;
                        break;
                    }
                }
            }
        }

        // Re-join tokens; separators are single spaces where the source had
        // whitespace, nothing where tokens were adjacent (e.g. "¬(...)").
        // Spaces directly inside group delimiters vanish by construction.
        inline std::string emit(const std::vector<Token>& toks)
        {
            std::string out;
            for (size_t i = 0; i < toks.size(); ++i)
            {
                if (i > 0 && toks[i].spaced) out += ' ';
                out += toks[i].text;
            }
            return out;
        }

        inline std::string parse_group(const std::string& s, size_t& pos);

        // Tokenize s from pos until `close` (or end of string); ()/{}
        // groups become single, already-contracted tokens.
        inline std::vector<Token> parse_sequence(const std::string& s, size_t& pos, const char close, bool& found_close)
        {
            std::vector<Token> toks;
            bool               spaced = false;
            found_close               = false;
            while (pos < s.size())
            {
                const char c = s[pos];
                if (c == ' ')
                {
                    spaced = true;
                    ++pos;
                    continue;
                }
                if (close != '\0' && c == close)
                {
                    ++pos;
                    found_close = true;
                    break;
                }
                if (c == '(' || c == '{')
                {
                    toks.push_back({parse_group(s, pos), spaced});
                }
                else
                {
                    const size_t start = pos;
                    while (pos < s.size() && s[pos] != ' '
                           && s[pos] != '(' && s[pos] != '{' && s[pos] != ')' && s[pos] != '}')
                        ++pos;
                    if (pos == start) ++pos; // stray ')' / '}': keep as its own token
                    toks.push_back({s.substr(start, pos - start), spaced});
                }
                spaced = false;
            }
            contract(toks);
            return toks;
        }

        inline std::string parse_group(const std::string& s, size_t& pos)
        {
            const char open  = s[pos];
            const char close = open == '(' ? ')' : '}';
            ++pos;
            bool        found = false;
            const auto  inner = parse_sequence(s, pos, close, found);
            std::string out(1, open);
            out += emit(inner);
            if (found) out += close;
            return out;
        }
    } // namespace sugar_detail

    inline std::string sugar_canonical(const std::string& s)
    {
        const std::string n     = normalize(s);
        size_t            pos   = 0;
        bool              found = false;
        const auto        toks  = sugar_detail::parse_sequence(n, pos, '\0', found);
        return sugar_detail::emit(toks);
    }

    // True if `text` contains `expected` in either rendering.
    inline bool text_contains_sugar_aware(const std::string& text, const std::string& expected)
    {
        const std::string n = normalize(text);
        const std::string e = normalize(expected);
        if (e.empty()) return false;
        if (n.find(e) != std::string::npos) return true;
        const std::string ce = sugar_canonical(expected);
        return !ce.empty() && sugar_canonical(text).find(ce) != std::string::npos;
    }

    // True if `text` starts with `expected` in either rendering.
    inline bool text_starts_with_sugar_aware(const std::string& text, const std::string& expected)
    {
        const std::string n = normalize(text);
        const std::string e = normalize(expected);
        if (e.empty()) return false;
        if (n.size() >= e.size() && n.compare(0, e.size(), e) == 0) return true;
        const std::string ce = sugar_canonical(expected);
        const std::string cn = sugar_canonical(text);
        return !ce.empty() && cn.size() >= ce.size() && cn.compare(0, ce.size(), ce) == 0;
    }

    // Return the text of the last non-empty Out-channel event.
    inline std::string last_out_text(const zelph::io::OutputCollector& collector)
    {
        const auto& events = collector.events();
        auto        rev    = std::ranges::reverse_view(events);
        auto        it     = std::ranges::find_if(rev, [](const auto& event)
                                                  { return event.channel == zelph::io::OutputChannel::Out && !event.text.empty(); });
        return it != rev.end() ? it->text : std::string{};
    }

    // Check whether the last Out line starts with the expected pattern
    // after whitespace normalization on both sides.
    [[maybe_unused]] inline bool last_output_starts_with(const zelph::io::OutputCollector& collector, const std::string& expected)
    {
        return text_starts_with_sugar_aware(last_out_text(collector), expected);
    }

    // Check whether ANY Out-channel event starts with the expected pattern (normalized).
    inline bool any_output_starts_with(const zelph::io::OutputCollector& collector, const std::string& expected)
    {
        return std::any_of(collector.events().begin(), collector.events().end(), [&](const auto& e)
                           { return e.channel == zelph::io::OutputChannel::Out
                                 && text_starts_with_sugar_aware(e.text, expected); });
    }

    // Check whether ANY Out-channel event contains the substring (normalized).
    inline bool any_output_contains(const zelph::io::OutputCollector& collector, const std::string& sub)
    {
        return std::any_of(collector.events().begin(), collector.events().end(), [&](const auto& e)
                           { return e.channel == zelph::io::OutputChannel::Out
                                 && text_contains_sugar_aware(e.text, sub); });
    }

    // Check whether ANY event in ANY channel contains the substring (normalized).
    inline bool any_event_contains(const zelph::io::OutputCollector& collector, const std::string& sub)
    {
        std::string exp = normalize(sub);
        if (exp.empty()) return false;
        for (const auto& e : collector.events())
        {
            if (normalize(e.text).find(exp) != std::string::npos)
                return true;
        }
        return false;
    }

    // Collect all "Answer:" lines as normalized answer text (prefix stripped).
    inline std::vector<std::string> collect_answers(const zelph::io::OutputCollector& collector)
    {
        std::vector<std::string> answers;
        const std::string        prefix = "Answer:";
        for (const auto& e : collector.events())
        {
            if (e.channel != zelph::io::OutputChannel::Out) continue;
            std::string n = normalize(e.text);
            if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0)
            {
                std::string answer = n.substr(prefix.size());
                // Strip leading space after "Answer:"
                if (!answer.empty() && answer[0] == ' ')
                    answer = answer.substr(1);
                answers.push_back(answer);
            }
        }
        return answers;
    }

    // Check that a specific answer is present (order-independent, normalized).
    inline bool answers_contain(const zelph::io::OutputCollector& collector, const std::string& expected)
    {
        const std::string        exp = normalize(expected);
        const std::string        ce  = sugar_canonical(expected);
        std::vector<std::string> all = collect_answers(collector);
        return std::any_of(all.begin(), all.end(), [&](const auto& a)
                           { return a == exp || sugar_canonical(a) == ce; });
    }

    // Check that "Found one or more contradictions!" appears in any output channel.
    inline bool has_contradiction(const zelph::io::OutputCollector& collector)
    {
        return std::any_of(collector.events().begin(), collector.events().end(), [](const auto& e)
                           { return normalize(e.text).find("Found one or more contradictions!") != std::string::npos; });
    }

    // Process a multiline string, feeding each line to interactive.process().
    inline void process_lines(const zelph::console::Interactive& interactive, const std::string& script)
    {
        std::istringstream iss(script);
        std::string        line;
        while (std::getline(iss, line))
        {
            interactive.process(line);
        }
    }

    // Run a test function in both parallel and single-core mode.
    // The function receives collector and interactive references.
    //
    // Both modes run with `.semi-naive check`: after the delta drains,
    // classic verification passes re-run until quiescence, and run()
    // throws if delta seeding missed any derivation. Every test therefore
    // doubles as an equivalence test between semi-naive and classic
    // evaluation -- permanently, so regressions in either direction fail
    // loudly instead of silently changing results.
    template <typename F>
    void run_both_modes(F&& test_fn)
    {
        SUBCASE("parallel")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive check");
            collector.clear();
            test_fn(collector, interactive);
        }
        SUBCASE("single-core")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".parallel");
            interactive.process(".semi-naive check");
            collector.clear();
            test_fn(collector, interactive);
        }
    }

    // Run a test against every arithmetic stdlib module, nested into
    // run_both_modes: 3 modules x 2 parallelism modes per leaf subcase,
    // each in `.semi-naive check` mode. All three modules expose the
    // identical decimal &-literal interface on identical predicates, so
    // tests written against that interface are representation-agnostic
    // and should use this helper. For binary-nand-arithmetic every run
    // additionally exercises the stratified NAF gate bootstrap, and
    // check mode extends the delta/classic equivalence guarantee to that
    // module's deferred stratum.
    //
    // NOT suitable for tests that inspect the internal digit
    // representation (raw <...> lists differ per module, e.g. after
    // (zelph/set-number-digits [])) or that pin module-specific behavior.
    //
    // The collector is cleared after the import, so module-dependent
    // load-time echoes (rule definitions, derived gate tables) cannot
    // leak into positive or negative output checks.
    template <typename F>
    void run_arithmetic_modules(F&& test_fn)
    {
        auto with_module = [&](const char* module)
        {
            run_both_modes([&](auto& collector, auto& interactive)
                           {
                    interactive.process(std::string(".import ") + module);
                    collector.clear();
                    test_fn(collector, interactive); });
        };

        SUBCASE("arithmetic") { with_module("arithmetic"); }
        SUBCASE("binary-arithmetic") { with_module("binary-arithmetic"); }
        SUBCASE("binary-nand-arithmetic") { with_module("binary-nand-arithmetic"); }
    }

    // Helper: run test with logging enabled at depth 1 for diagnostics.
    // Usage:  run_both_modes_logged([](auto& collector, auto& interactive) { ... });
    template <typename F>
    void run_both_modes_logged(F&& test_fn)
    {
        SUBCASE("parallel (logged)")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive check");
            interactive.process(".log 1");
            collector.clear();
            test_fn(collector, interactive);
        }
        SUBCASE("single-core (logged)")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".parallel");
            interactive.process(".semi-naive check");
            interactive.process(".log 1");
            collector.clear();
            test_fn(collector, interactive);
        }
    }
} // namespace zelph::test