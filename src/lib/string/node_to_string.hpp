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

#include "network/network_types.hpp"

#include <zelph_export.h>

#include <memory>
#include <string>
#include <unordered_set>

namespace zelph::network
{
    class Zelph;
    struct DisplayTables;
}

namespace zelph::string
{
    static constexpr int default_display_max_neighbors{5};

    // Per-node state for script-registered display schemes (see
    // network::DisplayScheme). Callers of node_to_string never construct
    // this -- the top-level call creates the root context and the recursion
    // threads it through. The IN fields describe the PARENT; the OUT fields
    // are the child's report back.
    struct SchemeContext
    {
        const network::DisplayTables* tables{nullptr}; // snapshot, fetched once per top-level call

        // IN: the parent operator, when the parent renders under a scheme.
        std::size_t scheme{0};
        int         precedence{0};
        int         assoc{-1};
        bool        right_side{false};
        bool        active{false};

        // IN: the parent supplies delimiters of its own around this operand
        // (an application's argument list), so no parentheses may be added.
        bool enclosed{false};

        // IN: forbid scheme rendering for this node (bail-out re-render).
        bool no_scheme{false};

        // IN: the caller writes the parentheses around this statement
        // directly (`¬(...)`), so it gets none as a value within another
        // one.
        bool bare_statement{false};

        // IN: the node represents a query's condition, typed on its
        // own line (condition_to_string); solely the top-level call
        // reads it.
        bool typed_condition{false};

        // IN: the node resides at the location where a condition is
        // written, and solely rules and condition lists are positioned
        // above it: the condition within a rule, a member of its
        // condition list or of a query's, or the operand within a negation
        // standing there. A path or neural condition then prints as it is
        // typed at that location.
        bool condition_slot{false};

        // IN: the node serves as the operand within a path condition
        // whose representation appears as `S p⁺ O`, and its predicate
        // bears this marker.
        const char* path_marker{nullptr};

        // OUT: this node is writable in the parent's scheme.
        bool expressible{false};
        // OUT: the rendering used forms the default renderer would not have
        // produced (elided parentheses, foreign numeral prefix).
        bool deviated{false};
        // OUT: the result is a self-delimiting token; do not parenthesize it.
        bool self_delimited{false};
        // OUT: the result is a bare identifier -- required in the head
        // position of an application form.
        bool atomic{false};
    };

    // As long as one remains alive, this thread renders the premises of a
    // deduction or of a proof, which name the facts a firing has discovered: a
    // path or a neural condition prints as the fact it is,
    // `(S p O) closure one-or-more` or `(P) nn net`, outside a condition slot.
    // In all other locations, it appears exactly as it is typed, `S p⁺ O` or
    // `≈net(P)`.
    struct PremiseRendering
    {
        PremiseRendering();
        ~PremiseRendering();
        PremiseRendering(const PremiseRendering&)            = delete;
        PremiseRendering& operator=(const PremiseRendering&) = delete;
    };

    network::Node last_node_to_string_node();
    void          reset_last_node();
    void          set_last_node(network::Node node);
    void          node_to_string(const network::Zelph* const z, std::string& result, const std::string& lang, network::Node node, const int max_objects = default_display_max_neighbors, const network::Variables& variables = {}, network::Node parent = 0, std::shared_ptr<std::unordered_set<network::Node>> history = nullptr, SchemeContext* ctx = nullptr);
    bool          is_inside_node_to_wstring();
    bool          is_var(std::string token);

    /// A query's condition, expressed exactly as entered: a negated
    /// condition appearing on its own line prints as `¬(P)`, just like in a
    /// rule's condition slot, where node_to_string writes the fact it is,
    /// `(P) ~ negation` -- the echo and response to "¬(X p b)" then re-enter
    /// as the identical query. A path or neural condition prints as `S p⁺ O`
    /// or `≈net(P)`, and a condition list as `A, B`, with its members
    /// formatted identically. Every other node, and any node possessing a
    /// parent, prints as node_to_string prints it.
    void condition_to_string(const network::Zelph* const z, std::string& result, const std::string& lang, network::Node node, const int max_objects = default_display_max_neighbors, const network::Variables& variables = {}, network::Node parent = 0);

    /// Is the self-fact sugar ":pred subject" re-enterable for this
    /// predicate NAME? Display and the test helpers ask the same function,
    /// so the two cannot drift apart.
    ZELPH_EXPORT bool selffact_sugar_safe(const std::string& name);
}
