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

#include "numeric_order.hpp"

#include "fact_structure.hpp"
#include "zelph.hpp"

namespace zelph::network
{
    namespace
    {
        // The depth handed to get_fact_structures, which uses it solely to
        // decide whether to log: sufficient to prevent a trace at `.log 1`
        // or `.log 2` from being overwhelmed by the cells of every number
        // being compared.
        constexpr int structure_log_depth = 4;

        // What a node of a term stands for. Under bindings a variable stands
        // for its value, or for nothing (0) when it lacks one; every other
        // node, and every node of a term read without bindings, stands for
        // itself.
        Node stands_for(const Variables* const bindings, const Node node)
        {
            if (bindings == nullptr || !Zelph::is_var(node)) return node;
            const auto it = bindings->find(node);
            return it == bindings->end() || Zelph::is_var(it->second) ? 0 : it->second;
        }

        // The digits of the cons list starting at `first`, whose fact
        // structures the caller has already read. Stored in
        // least-significant-digit-first order, and cut after the most
        // significant nonzero digit: the zeros above it are the zero extension
        // `cmp` applies to the shorter operand, so <007> reads as 7.
        //
        // Cells are read as the &-display interprets them (via node_to_string),
        // with the exception that a cell must contain precisely one object,
        // whereas the display takes the first: every cell a cons fact, every
        // car a registered digit, the chain ending at nil. Any other
        // configuration is not a numeral -- an atom in a car, a chain ending at
        // an atom, a variable.
        //
        // Hash-consing builds a chain starting from nil and upward, and a name
        // merge refuses to close a cycle (merge_shape's occurs check), so only
        // a network saved before that check, or a damaged file, can hold a
        // circle. No user pathway accesses such a case, hence no test exists,
        // but a walk that never meets nil must still end: Brent's cycle
        // detection method compares each cell with one retained at powers of
        // two, and costs no second decomposition.
        bool read_digits(const Zelph&                              z,
                         const std::unordered_map<Node, uint32_t>& digits,
                         const Variables* const                    bindings,
                         const Node                                first,
                         FactStructurePtr                          structures,
                         std::vector<uint32_t>&                    out)
        {
            out.clear();
            std::size_t significant = 0;
            Node        checkpoint  = first;
            std::size_t steps       = 0;
            std::size_t period      = 1;

            for (;;)
            {
                if (structures->empty()) return false;
                const FactStructure& cell = structures->front();
                if (stands_for(bindings, cell.predicate) != z.core.Cons || cell.objects.size() != 1) return false;

                const auto digit = digits.find(stands_for(bindings, cell.subject));
                if (digit == digits.end()) return false;
                out.push_back(digit->second);
                if (digit->second != 0) significant = out.size();

                const Node next = stands_for(bindings, *cell.objects.begin());
                if (next == z.core.Nil) break;
                if (next == checkpoint || !Zelph::is_hash(next) || Zelph::is_var(next)) return false;
                if (++steps == period)
                {
                    checkpoint = next;
                    period *= 2;
                    steps = 0;
                }
                structures = get_fact_structures(&z, next, structure_log_depth);
            }

            out.resize(significant);
            return true;
        }

        // -1, 0, or 1 for two magnitudes in the form read_digits returns.
        int compare_magnitudes(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
        {
            if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
            for (std::size_t i = a.size(); i-- > 0;)
                if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
            return 0;
        }

        // -1, 0, or 1: the sign of the value, a signed zero equating to zero.
        int sign_of(const DecodedNumber& n)
        {
            if (n.digits.empty()) return 0;
            return n.negative ? -1 : 1;
        }

        // Reads a node, or under bindings a term of a rule, as a number. A
        // structure is read via what its components stand for, regardless of
        // whether the fact they would make exists: its value is independent of
        // that.
        bool decode_term(const Zelph& z, const NumberVocabulary& vocabulary, const Node term, const Variables* const bindings, DecodedNumber& out)
        {
            out.is_number = false;
            out.negative  = false;
            out.digits.clear();

            // Without a digit alphabet, nothing counts as a number: a list
            // containing the characters 1 and 2 remains two distinct atoms
            // within a list until a substrate defines them as digits, as it
            // does for the display.
            if (!vocabulary.has_digits()) return false;

            const Node node = stands_for(bindings, term);
            if (node == z.core.Nil)
            {
                out.is_number = true;
                return true;
            }
            if (!Zelph::is_hash(node) || Zelph::is_var(node)) return false;

            const FactStructurePtr structures = get_fact_structures(&z, node, structure_log_depth);
            if (structures->empty()) return false;
            const Node predicate = stands_for(bindings, structures->front().predicate);

            if (predicate == z.core.Cons)
            {
                if (!read_digits(z, *vocabulary.digits, bindings, node, structures, out.digits)) return false;
                out.is_number = true;
                return true;
            }

            if (vocabulary.sign_relation != 0 && predicate == vocabulary.sign_relation)
            {
                const FactStructure& signed_term = structures->front();
                if (signed_term.objects.size() != 1) return false;

                const Node sign     = stands_for(bindings, signed_term.subject);
                const bool negative = vocabulary.negative != 0 && sign == vocabulary.negative;
                if (!negative && (vocabulary.positive == 0 || sign != vocabulary.positive)) return false;

                const Node magnitude = stands_for(bindings, *signed_term.objects.begin());
                if (magnitude != z.core.Nil)
                {
                    if (!Zelph::is_hash(magnitude) || Zelph::is_var(magnitude)) return false;
                    if (!read_digits(z, *vocabulary.digits, bindings, magnitude, get_fact_structures(&z, magnitude, structure_log_depth), out.digits)) return false;
                }

                out.is_number = true;
                out.negative  = negative && !out.digits.empty();
                return true;
            }

            // Decimal terms, (M dec K), are read here once a module writes them:
            // the scale K beside the digits of M, which compare_decoded
            // subsequently aligns to a common scale.
            return false;
        }
    }

    bool decode_number(const Zelph& z, const NumberVocabulary& vocabulary, const Node node, DecodedNumber& out)
    {
        return decode_term(z, vocabulary, node, nullptr, out);
    }

    NumericOrder compare_decoded(const DecodedNumber& a, const DecodedNumber& b)
    {
        if (!a.is_number || !b.is_number) return NumericOrder::none;

        const int sa = sign_of(a);
        const int sb = sign_of(b);
        if (sa != sb) return sa < sb ? NumericOrder::less : NumericOrder::greater;
        if (sa == 0) return NumericOrder::equal;

        int order = compare_magnitudes(a.digits, b.digits);
        if (sa < 0) order = -order;
        if (order == 0) return NumericOrder::equal;
        return order < 0 ? NumericOrder::less : NumericOrder::greater;
    }

    NumericOrder compare_numbers(const Zelph& z, const NumberVocabulary& vocabulary, const Node a, const Node b)
    {
        DecodedNumber da;
        if (!decode_number(z, vocabulary, a, da)) return NumericOrder::none;
        DecodedNumber db;
        if (!decode_number(z, vocabulary, b, db)) return NumericOrder::none;
        return compare_decoded(da, db);
    }

    NumericOrder compare_terms(const Zelph& z, const NumberVocabulary& vocabulary, const Node a, const Node b, const Variables& bindings)
    {
        DecodedNumber da;
        if (!decode_term(z, vocabulary, a, &bindings, da)) return NumericOrder::none;
        DecodedNumber db;
        if (!decode_term(z, vocabulary, b, &bindings, db)) return NumericOrder::none;
        return compare_decoded(da, db);
    }
}
