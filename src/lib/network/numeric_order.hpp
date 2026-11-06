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

#include "network_types.hpp"

#include <zelph_export.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zelph::network
{
    class Zelph;

    // The order of two numbers based on their values. It is total across
    // numbers: `none` indicates solely that an operand is not a number. In
    // every instance where the standard library's `cmp` derives a verdict for
    // two terms, this is that verdict, and the tests hold it there
    // (numeric_compare_oracle.hpp); which of the pairs `cmp` routes at all is
    // governed by the rules, and nothing here repeats it.
    enum class NumericOrder : std::uint8_t
    {
        none,
        less,
        equal,
        greater
    };

    // How the loaded modules write numbers, as they have registered it. Nothing
    // about this is known to the engine otherwise: the digits come from
    // zelph/set-number-digits (the substrate), and the signed form comes from
    // zelph/set-number-signs (integer-arithmetic.zph). A part that is not
    // registered reads as "no number of that kind". Zelph::number_vocabulary
    // provides a snapshot, which later registrations do not alter.
    struct NumberVocabulary
    {
        // The digit alphabet, digit node -> value; its size corresponds to the
        // base.
        std::shared_ptr<const std::unordered_map<Node, uint32_t>> digits;
        // A signed number constitutes the fact (S R M): R the sign relation, S
        // one of the two sign atoms, M a natural.
        Node sign_relation{0};
        Node positive{0};
        Node negative{0};

        // Without digits nothing is a number.
        bool has_digits() const { return digits && !digits->empty(); }
    };

    // A number retrieved by reading its node, which is one of
    //
    // a natural  a nil-terminated cons list of registered digits, least
    //            significant first, or nil itself -- the exhausted list that
    //            every digit recursion ends at, which reads as zero;
    // an integer (S R M) under the registered signs, M a natural: (pos zint N)
    //            reads as N and (neg zint N) as -N.
    //
    // What persists from the form is the value, so two forms sharing the same
    // value decode alike. The extension point for decimal terms -- (M dec K),
    // which no module writes at present -- would serve as a scale here:
    // decode_number would read it after the two preceding ones, and
    // compare_decoded would align two of them to a shared scale before
    // comparing their digits.
    struct DecodedNumber
    {
        bool is_number{false};
        bool negative{false}; // true exclusively for a nonzero value: zero lacks a sign
        // The digits of the value, listed in reverse order starting with
        // the least significant, excluding any zeros above the most
        // significant nonzero digit: <007> and &7 both yield {7}. Empty
        // when the value is zero.
        std::vector<uint32_t> digits;
    };

    // Reads `node` as a numeric value. False, and `out.is_number` false, when
    // it is none. Reads the graph and the vocabulary exclusively: thread-safe,
    // creates nothing, and calls no script.
    ZELPH_EXPORT bool decode_number(const Zelph& z, const NumberVocabulary& vocabulary, Node node, DecodedNumber& out);

    // The order of two decoded numbers by value, `none` when either one is not
    // a number.
    ZELPH_EXPORT NumericOrder compare_decoded(const DecodedNumber& a, const DecodedNumber& b);

    // Apply decode_number to each operand, then compare_decoded.
    ZELPH_EXPORT NumericOrder compare_numbers(const Zelph& z, const NumberVocabulary& vocabulary, Node a, Node b);

    // The same applies to two terms of a rule under its bindings. A variable
    // represents its value, even when embedded within a structure, and a term is
    // read by value: no fact needs to exist for it, so (neg zint A) under A = &3
    // reads as -3 regardless of whether the graph holds that statement. A
    // variable lacking a value reads as no number.
    ZELPH_EXPORT NumericOrder compare_terms(const Zelph& z, const NumberVocabulary& vocabulary, Node a, Node b, const Variables& bindings);

    // The comparison conditions a rule writes, `A zelph:lt B` and its five
    // siblings. Each constitutes a name within the language zelph that the
    // engine reads (Zelph::engine_vocabulary) and caches once a node carries it
    // (Reasoning::refresh_comparison_predicates).
    enum class NumericRelation : std::uint8_t
    {
        lt,
        le,
        gt,
        ge,
        eq,
        ne,
        none
    };

    constexpr std::size_t numeric_relation_count = 6;

    // Indexed by NumericRelation.
    constexpr std::array<const char*, numeric_relation_count> numeric_relation_names{
        "zelph:lt", "zelph:le", "zelph:gt", "zelph:ge", "zelph:eq", "zelph:ne"};

    inline const char* numeric_relation_name(const NumericRelation relation)
    {
        return relation == NumericRelation::none ? "" : numeric_relation_names[static_cast<std::size_t>(relation)];
    }

    // The relation a name represents, `none` for every other name.
    // Each of the six starts with "zelph:", whereas no name from the
    // standard library does, so most names incur a single failed prefix
    // test.
    inline NumericRelation numeric_relation_named(const std::string_view name)
    {
        if (!name.starts_with("zelph:")) return NumericRelation::none;
        for (std::size_t i = 0; i < numeric_relation_count; ++i)
            if (name == numeric_relation_names[i]) return static_cast<NumericRelation>(i);
        return NumericRelation::none;
    }

    // Whether `relation` holds for two numbers in `order`. No relation
    // holds without a verdict, `ne` included: two terms that are not both
    // numbers are not numerically different either.
    inline bool numeric_relation_holds(const NumericRelation relation, const NumericOrder order)
    {
        switch (relation)
        {
        case NumericRelation::lt:
            return order == NumericOrder::less;
        case NumericRelation::le:
            return order == NumericOrder::less || order == NumericOrder::equal;
        case NumericRelation::gt:
            return order == NumericOrder::greater;
        case NumericRelation::ge:
            return order == NumericOrder::greater || order == NumericOrder::equal;
        case NumericRelation::eq:
            return order == NumericOrder::equal;
        case NumericRelation::ne:
            return order == NumericOrder::less || order == NumericOrder::greater;
        case NumericRelation::none:
            return false;
        }
        return false;
    }

    // The relation that holds for two numbers exactly when `relation`
    // does not: what the negation of the condition would state regarding
    // numbers.
    inline NumericRelation numeric_relation_complement(const NumericRelation relation)
    {
        switch (relation)
        {
        case NumericRelation::lt:
            return NumericRelation::ge;
        case NumericRelation::le:
            return NumericRelation::gt;
        case NumericRelation::gt:
            return NumericRelation::le;
        case NumericRelation::ge:
            return NumericRelation::lt;
        case NumericRelation::eq:
            return NumericRelation::ne;
        case NumericRelation::ne:
            return NumericRelation::eq;
        case NumericRelation::none:
            return NumericRelation::none;
        }
        return NumericRelation::none;
    }
}
