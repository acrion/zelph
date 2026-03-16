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

#include <unordered_set>

namespace zelph::network
{
    class Zelph;
}

namespace zelph::string
{
    static constexpr int default_display_max_neighbors{5};

    void node_to_string(const network::Zelph* const z, std::string& result, const std::string& lang, network::Node fact, const int max_objects = default_display_max_neighbors, const network::Variables& variables = {}, network::Node parent = 0, std::shared_ptr<std::unordered_set<network::Node>> history = nullptr);
    bool is_inside_node_to_wstring();
    bool is_var(std::string token);
}
