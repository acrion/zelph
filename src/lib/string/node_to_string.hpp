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

    network::Node last_node_to_string_node();
    void          reset_last_node();
    void          node_to_string(const network::Zelph* const z, std::string& result, const std::string& lang, network::Node node, const int max_objects = default_display_max_neighbors, const network::Variables& variables = {}, network::Node parent = 0, std::shared_ptr<std::unordered_set<network::Node>> history = nullptr, SchemeContext* ctx = nullptr);
    bool          is_inside_node_to_wstring();
    bool          is_var(std::string token);

    /// Is the self-fact sugar ":pred subject" re-enterable for this
    /// predicate NAME? Display and the test helpers ask the same function,
    /// so the two cannot drift apart.
    ZELPH_EXPORT bool selffact_sugar_safe(const std::string& name);
}
