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

// The stdlib `cmp` serves as the oracle for every comparison the engine
// performs independently.
//
// The engine compares numbers without relying on arithmetic rules in two
// places, or will do so: the comparison conditions (`A zelph:lt B`,
// reasoning_compare.cpp) and the order keys within the SPARQL layer. Both
// establish a total order based on numerical value, which is required for
// sorting, while `cmp` is partial. Thus, whenever `cmp` derives a verdict, the
// rules serve as the definitive standard, and each implementation is held to
// them by running them; when `cmp` derives none, the pair still possesses an
// order, and the author of the terms is aware of it:
//
//   stdlib_cmp_oracle      states `A cmp B` for every case, runs the rules,
//                          and reads back the verdict they derived (or none);
//   check_against_stdlib   holds any comparator Node x Node -> NumericOrder
//                          to that verdict, on the cases that have one;
//   check_against_values   holds it to the order by value each case names, on
//                          every case;
//   check_conditions_against_values
//                          does the same for the six conditions, evaluated as
//                          rule conditions by the engine;
//   check_every_verdict_occurs
//                          guards a case list against passing these checks
//                          unseen.
//
// A case whose named order contradicts `cmp` fails one of the two checks
// regardless of the comparator's response, so together they hold the cases
// as well.
//
// The cases are terms expressed in zelph syntax. A generator writes numbers, in
// the forms that the modules read, and a manually composed list can include
// terms that are none.

#include "test_helpers.hpp"

#include "network/numeric_order.hpp"
#include "network/reasoning.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace zelph::test
{
    // Two terms in zelph syntax, as they would be typed: `&123`, `<0071>`,
    // `nil`, `(neg zint &5)`, and their order by value, `none` if any of them
    // is no number. The writer of the terms knows it without the engine, based
    // on the values they were written from.
    struct NumericCase
    {
        std::string           lhs;
        std::string           rhs;
        network::NumericOrder by_value{network::NumericOrder::none};
    };

    // One case as the oracle saw it: the nodes its terms denote, and the
    // verdict `cmp` derived from them.
    struct NumericOracleEntry
    {
        NumericCase           terms;
        network::Node         lhs{0};
        network::Node         rhs{0};
        network::NumericOrder stdlib{network::NumericOrder::none};
    };

    inline std::string numeric_order_text(const network::NumericOrder order)
    {
        switch (order)
        {
        case network::NumericOrder::less:
            return "less";
        case network::NumericOrder::equal:
            return "equal";
        case network::NumericOrder::greater:
            return "greater";
        case network::NumericOrder::none:
            return "none";
        }
        return "?";
    }

    namespace numeric_oracle_detail
    {
        // Each answer's bindings for `query`, taken from the engine rather than
        // from the printed lines (Reasoning::set_query_collector).
        inline std::vector<std::shared_ptr<network::Variables>> answers(const console::Interactive& interactive, const std::string& query)
        {
            std::vector<std::shared_ptr<network::Variables>> results;
            network::Reasoning* const                        graph = interactive.graph();
            graph->set_query_collector(&results);
            try
            {
                interactive.process(query);
            }
            catch (...)
            {
                graph->set_query_collector(nullptr);
                throw;
            }
            graph->set_query_collector(nullptr);
            return results;
        }

        // The case index a node named "ncase<i>" stands for, or -1.
        inline long case_index(const console::Interactive& interactive, const network::Node node)
        {
            const std::string name = interactive.graph()->get_name(node, "zelph", false);
            if (!name.starts_with("ncase")) return -1;
            return std::stol(name.substr(5));
        }

        // The case an answer concerns: the value that variable C took,
        // the one node named "ncase<i>" among the values. Read by the
        // values since the variables themselves lack a name a test could
        // ask for -- every statement quantifies variables belonging to itself.
        inline long case_of(const console::Interactive& interactive, const network::Variables& bindings)
        {
            for (const auto& [variable, value] : bindings)
                if (const long i = case_index(interactive, value); i >= 0) return i;
            return -1;
        }

        // In an answer to `C <predicate> T`, the value T took.
        inline network::Node other_than_case(const console::Interactive& interactive, const network::Variables& bindings)
        {
            for (const auto& [variable, value] : bindings)
                if (case_index(interactive, value) < 0) return value;
            return 0;
        }

        // For each case, whether the query, with C bound to the case,
        // produces an answer.
        inline std::vector<bool> cases_answering(const console::Interactive& interactive, const std::string& query, const std::size_t count)
        {
            std::vector<bool> answered(count, false);
            for (const auto& bindings : answers(interactive, query))
            {
                const long i = case_of(interactive, *bindings);
                if (i >= 0 && static_cast<std::size_t>(i) < count) answered[static_cast<std::size_t>(i)] = true;
            }
            return answered;
        }
    }

    // The nodes `terms` denote, in sequence. Each term i is expressed as the
    // fact `ncase<i> ncl <term>`, which finds its node once more; no execution
    // occurs beyond what the session runs for any given statement.
    inline std::vector<network::Node> nodes_of_terms(const console::Interactive& interactive, const std::vector<std::string>& terms)
    {
        for (std::size_t i = 0; i < terms.size(); ++i)
            interactive.process("ncase" + std::to_string(i) + " ncl " + terms[i]);

        std::vector<network::Node> nodes(terms.size(), 0);
        for (const auto& bindings : numeric_oracle_detail::answers(interactive, "C ncl T"))
        {
            const long i = numeric_oracle_detail::case_of(interactive, *bindings);
            REQUIRE(i >= 0);
            REQUIRE(static_cast<std::size_t>(i) < nodes.size());
            nodes[static_cast<std::size_t>(i)] = numeric_oracle_detail::other_than_case(interactive, *bindings);
        }
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            INFO(terms[i]);
            REQUIRE(nodes[i] != 0);
        }
        return nodes;
    }

    // Runs stdlib `cmp` across `cases` within the session `interactive`,
    // which must have the substrate imported -- and integer-arithmetic, if
    // a case holds a signed term. Each case i becomes the two facts
    // `ncase<i> ncl <lhs>` and `ncase<i> ncr <rhs>`, which find its nodes
    // once more, and the statement `<lhs> cmp <rhs>`; one run then derives
    // every verdict. The verdict is the relational fact the rules derive,
    // `<`, `>` or `==`, and the oracle refuses to answer (a failed REQUIRE)
    // when the rules derive two such facts, or a result under `=` that
    // disagrees.
    inline std::vector<NumericOracleEntry> stdlib_cmp_oracle(const console::Interactive& interactive, const std::vector<NumericCase>& cases)
    {
        using numeric_oracle_detail::case_of;
        using numeric_oracle_detail::cases_answering;
        using numeric_oracle_detail::other_than_case;

        // A single run encompasses all of them: auto-run would run once per
        // statement.
        const bool auto_run = interactive.is_auto_run_active();
        if (auto_run) interactive.process(".auto-run");
        for (std::size_t i = 0; i < cases.size(); ++i)
        {
            const std::string id = "ncase" + std::to_string(i);
            interactive.process(id + " ncl " + cases[i].lhs);
            interactive.process(id + " ncr " + cases[i].rhs);
            interactive.process(cases[i].lhs + " cmp " + cases[i].rhs);
        }
        interactive.process(".run");
        if (auto_run) interactive.process(".auto-run");

        std::vector<NumericOracleEntry> oracle(cases.size());
        for (std::size_t i = 0; i < cases.size(); ++i)
            oracle[i].terms = cases[i];

        for (const bool left : {true, false})
            for (const auto& bindings : numeric_oracle_detail::answers(interactive, left ? "C ncl T" : "C ncr T"))
            {
                const long i = case_of(interactive, *bindings);
                REQUIRE(i >= 0);
                REQUIRE(static_cast<std::size_t>(i) < oracle.size());
                (left ? oracle[static_cast<std::size_t>(i)].lhs : oracle[static_cast<std::size_t>(i)].rhs) = other_than_case(interactive, *bindings);
            }

        const std::vector<bool> less    = cases_answering(interactive, "C ncl A, C ncr B, A < B", cases.size());
        const std::vector<bool> greater = cases_answering(interactive, "C ncl A, C ncr B, A > B", cases.size());
        const std::vector<bool> equal   = cases_answering(interactive, "C ncl A, C ncr B, A == B", cases.size());

        // The outcome that the rules expose under `=`, the second half of their
        // contract (rule CC0 and the facade's bridges): which of lt, eq, gt
        // a case answers to.
        std::map<std::size_t, std::string> result;
        for (const char* const name : {"lt", "eq", "gt"})
        {
            const std::vector<bool> answering = cases_answering(interactive, std::string("C ncl A, C ncr B, (A cmp B) = ") + name, cases.size());
            for (std::size_t i = 0; i < answering.size(); ++i)
                if (answering[i]) result[i] += (result[i].empty() ? "" : " ") + std::string(name);
        }

        for (std::size_t i = 0; i < oracle.size(); ++i)
        {
            INFO("case " << i << ": " << cases[i].lhs << " cmp " << cases[i].rhs);
            REQUIRE(oracle[i].lhs != 0);
            REQUIRE(oracle[i].rhs != 0);
            REQUIRE(int(less[i]) + int(greater[i]) + int(equal[i]) <= 1);

            if (less[i]) oracle[i].stdlib = network::NumericOrder::less;
            if (greater[i]) oracle[i].stdlib = network::NumericOrder::greater;
            if (equal[i]) oracle[i].stdlib = network::NumericOrder::equal;

            const auto        r        = result.find(i);
            const std::string expected = oracle[i].stdlib == network::NumericOrder::less    ? "lt"
                                       : oracle[i].stdlib == network::NumericOrder::greater ? "gt"
                                       : oracle[i].stdlib == network::NumericOrder::equal   ? "eq"
                                                                                            : "";
            REQUIRE((r == result.end() ? std::string() : r->second) == expected);
        }
        return oracle;
    }

    // Absence of vacuity in a case list under inspection: a list that lacks any
    // verdict of a particular type, or an oracle that retrieved nothing upon
    // reading back, would pass the checks undetected. `cmp` must likewise
    // remain silent on certain pairs regarding the order of their values: these
    // are precisely the pairs that the comparison adds to `cmp`.
    inline void check_every_verdict_occurs(const std::vector<NumericOracleEntry>& oracle)
    {
        using Order = network::NumericOrder;

        std::size_t verdicts[4]   = {0, 0, 0, 0};
        std::size_t only_by_value = 0;
        for (const auto& e : oracle)
        {
            ++verdicts[static_cast<int>(e.stdlib)];
            if (e.stdlib == Order::none && e.terms.by_value != Order::none) ++only_by_value;
        }
        CHECK(verdicts[static_cast<int>(Order::less)] > 0);
        CHECK(verdicts[static_cast<int>(Order::equal)] > 0);
        CHECK(verdicts[static_cast<int>(Order::greater)] > 0);
        CHECK(only_by_value > 0);
    }

    // Holds `comparator` (Node, Node) -> NumericOrder to the oracle: the
    // identical verdict in every case where `cmp` derived one. In cases where
    // it derived none, it remains silent on the matter of the comparator.
    template <typename Comparator>
    void check_against_stdlib(const std::vector<NumericOracleEntry>& oracle, Comparator&& comparator)
    {
        for (std::size_t i = 0; i < oracle.size(); ++i)
        {
            const NumericOracleEntry& e = oracle[i];
            if (e.stdlib == network::NumericOrder::none) continue;
            INFO("case " << i << ": " << e.terms.lhs << " vs " << e.terms.rhs << ", cmp derived " << numeric_order_text(e.stdlib));
            CHECK(numeric_order_text(comparator(e.lhs, e.rhs)) == numeric_order_text(e.stdlib));
        }
    }

    // Holds `comparator` to the order by value each case names, on every case.
    template <typename Comparator>
    void check_against_values(const std::vector<NumericOracleEntry>& oracle, Comparator&& comparator)
    {
        for (std::size_t i = 0; i < oracle.size(); ++i)
        {
            const NumericOracleEntry& e = oracle[i];
            INFO("case " << i << ": " << e.terms.lhs << " vs " << e.terms.rhs << ", by value " << numeric_order_text(e.terms.by_value));
            CHECK(numeric_order_text(comparator(e.lhs, e.rhs)) == numeric_order_text(e.terms.by_value));
        }
    }

    // Holds the six comparison conditions to the order by value: each one,
    // written as the condition in a rule governing the case's two terms,
    // must fire precisely for the cases whose order it accepts
    // (numeric_relation_holds) -- `A zelph:lt B` precisely when A's value
    // is less than B's, `A zelph:ne B` precisely when the two values are
    // different.
    inline void check_conditions_against_values(const console::Interactive& interactive, const std::vector<NumericOracleEntry>& oracle)
    {
        // The six rules in one run, as the oracle's statements: in check
        // mode, each run repeats a classic pass over every rule currently
        // in effect, including the arithmetic.
        const auto marker_of = [](const std::size_t r)
        { return "nheld-" + std::string(network::numeric_relation_names[r]).substr(6); };

        const bool auto_run = interactive.is_auto_run_active();
        if (auto_run) interactive.process(".auto-run");
        for (std::size_t r = 0; r < network::numeric_relation_count; ++r)
            interactive.process("(C ncl A, C ncr B, A " + std::string(network::numeric_relation_names[r]) + " B) => (C " + marker_of(r) + " yes)");
        interactive.process(".run");
        if (auto_run) interactive.process(".auto-run");

        for (std::size_t r = 0; r < network::numeric_relation_count; ++r)
        {
            const std::string       relation = network::numeric_relation_names[r];
            const std::vector<bool> held     = numeric_oracle_detail::cases_answering(interactive, "C " + marker_of(r) + " yes", oracle.size());

            for (std::size_t i = 0; i < oracle.size(); ++i)
            {
                const NumericOracleEntry& e = oracle[i];
                INFO("case " << i << ": " << e.terms.lhs << " " << relation << " " << e.terms.rhs << ", by value " << numeric_order_text(e.terms.by_value));
                CHECK(held[i] == network::numeric_relation_holds(static_cast<network::NumericRelation>(r), e.terms.by_value));
            }
        }
    }

    // ------------------------------------------------------------------
    // Generators
    // ------------------------------------------------------------------

    // The digits of the decimal string `decimal` in base 2, arranged from most
    // significant to least significant, form a digit list written within angle
    // brackets: needed to write the same value with leading zeros.
    inline std::string decimal_to_binary(std::string decimal)
    {
        if (decimal.find_first_not_of('0') == std::string::npos) return "0";
        std::string bits;
        while (decimal.find_first_not_of('0') != std::string::npos)
        {
            std::string half;
            int         carry = 0;
            for (const char c : decimal)
            {
                const int d = carry * 10 + (c - '0');
                half.push_back(static_cast<char>('0' + d / 2));
                carry = d % 2;
            }
            bits.push_back(static_cast<char>('0' + carry));
            decimal = half;
        }
        return std::string(bits.rbegin(), bits.rend());
    }

    // A natural number chosen at random, consisting of `digits` decimal digits,
    // with no leading zero.
    inline std::string random_decimal(std::mt19937_64& rng, const std::size_t digits)
    {
        std::uniform_int_distribution<int> first(1, 9);
        std::uniform_int_distribution<int> any(0, 9);
        std::string                        s(1, static_cast<char>('0' + first(rng)));
        while (s.size() < digits)
            s.push_back(static_cast<char>('0' + any(rng)));
        return s;
    }

    // The `decimal` value expressed with `zeros` leading zeros as a list of the
    // substrate's digits (base 10 or 2), `<00123>`: the non-canonical numeral
    // that subtraction derives, which `cmp` compares by value.
    inline std::string with_leading_zeros(const std::string& decimal, const int base, const std::size_t zeros)
    {
        return "<" + std::string(zeros, '0') + (base == 10 ? decimal : decimal_to_binary(decimal)) + ">";
    }

    // Seeded random cases across every form of number the modules read: the
    // naturals spanning 1 to 30 decimal digits as &-literals, those same values
    // with leading zeros, zero as &0, <00>, and nil, and the integers from
    // integer-arithmetic.zph, (neg zint N), the signed zero (neg zint &0), and
    // the operand form (pos zint N), with nil serving as a magnitude among the
    // zeros. Pairs are drawn at random, and a third of them share a value: the
    // identical term twice, or one value expressed in two forms. Each case
    // names its order, taken from the decimal strings it was written from,
    // which no substrate has seen.
    inline std::vector<NumericCase> random_numeric_cases(const std::uint64_t seed, const std::size_t count, const int base)
    {
        std::mt19937_64                            rng(seed);
        std::uniform_int_distribution<std::size_t> length(1, 30);
        std::uniform_int_distribution<int>         pick(0, 99);

        struct Value
        {
            std::string magnitude; // decimal digits, no leading zero ("0" for zero)
            bool        negative;
        };
        const auto random_value = [&]() -> Value
        {
            const int kind = pick(rng);
            if (kind < 8) return {"0", kind < 3};
            return {random_decimal(rng, length(rng)), pick(rng) < 35};
        };
        const auto order_of = [](const Value& a, const Value& b)
        {
            const auto sign = [](const Value& v)
            { return v.magnitude == "0" ? 0 : (v.negative ? -1 : 1); };
            const int sa = sign(a);
            const int sb = sign(b);
            if (sa != sb) return sa < sb ? network::NumericOrder::less : network::NumericOrder::greater;
            if (sa == 0) return network::NumericOrder::equal;
            const int magnitudes = a.magnitude.size() != b.magnitude.size() ? (a.magnitude.size() < b.magnitude.size() ? -1 : 1)
                                                                            : a.magnitude.compare(b.magnitude);
            const int order      = sa * magnitudes;
            if (order == 0) return network::NumericOrder::equal;
            return order < 0 ? network::NumericOrder::less : network::NumericOrder::greater;
        };
        // One among the forms that write `v`.
        const auto form_of = [&](const Value& v) -> std::string
        {
            const int choice = pick(rng);
            if (v.negative)
            {
                if (choice < 70) return "(neg zint &" + v.magnitude + ")";
                return "(neg zint " + with_leading_zeros(v.magnitude, base, 1 + static_cast<std::size_t>(choice % 3)) + ")";
            }
            if (v.magnitude == "0")
            {
                if (choice < 30) return "&0";
                if (choice < 45) return "<00>";
                if (choice < 60) return "nil";
                if (choice < 75) return "(neg zint &0)";
                if (choice < 85) return "(pos zint &0)";
                return choice < 93 ? "(neg zint nil)" : "(pos zint nil)";
            }
            if (choice < 70) return "&" + v.magnitude;
            if (choice < 85) return with_leading_zeros(v.magnitude, base, 1 + static_cast<std::size_t>(choice % 3));
            return "(pos zint &" + v.magnitude + ")";
        };

        std::vector<NumericCase> cases;
        cases.reserve(count);
        while (cases.size() < count)
        {
            const Value a = random_value();
            if (pick(rng) < 33)
            {
                const std::string first = form_of(a);
                cases.push_back({first, pick(rng) < 50 ? first : form_of(a), network::NumericOrder::equal});
                continue;
            }
            // A neighbour of identical length at times: the digits from the
            // most significant end must decide, not the length. Two
            // digits move in opposite directions, the higher one way and the
            // lower the other, causing a scan from the incorrect end to
            // yield the opposite verdict; with a single digit changed,
            // either end finds it.
            Value b = random_value();
            if (pick(rng) < 30 && a.magnitude.size() > 1)
            {
                b.magnitude          = a.magnitude;
                const std::size_t hi = std::uniform_int_distribution<std::size_t>(0, b.magnitude.size() - 2)(rng);
                const std::size_t lo = std::uniform_int_distribution<std::size_t>(hi + 1, b.magnitude.size() - 1)(rng);
                if (b.magnitude[hi] < '9' && b.magnitude[lo] > '0')
                {
                    ++b.magnitude[hi];
                    --b.magnitude[lo];
                }
                else if (b.magnitude[hi] > '0' && b.magnitude[lo] < '9')
                {
                    --b.magnitude[hi];
                    ++b.magnitude[lo];
                }
                if (b.magnitude[0] == '0') b.magnitude[0] = '1';
            }
            cases.push_back({form_of(a), form_of(b), order_of(a, b)});
        }
        return cases;
    }
}
