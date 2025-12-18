/*
Copyright (c) 2025 acrion innovations GmbH
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

#include "network.hpp"
#include "zelph.hpp"

#include <boost/serialization/map.hpp>
#include <boost/serialization/string.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace zelph
{
    namespace network
    {
        // cppcheck-suppress noConstructor
        class ZELPH_EXPORT Zelph::Impl : public Network
        {
            friend class Zelph;
            Impl() = default;

            friend class boost::serialization::access;

            template <class Archive>
            void serialize(Archive& ar, const unsigned int version)
            {
                ar& boost::serialization::base_object<Network>(*this);
                ar & _name_of_node;
                ar & _node_of_name;
            }

            // cannot use boost:bitmap because we have to support nodes having the same name (within one language). In this case, _node_of_name has no entries for all of these names.
            std::map<std::string, std::map<Node, std::wstring>> _name_of_node; // key is language identifier
            std::map<std::string, std::map<std::wstring, Node>> _node_of_name; // key is language identifier

            mutable std::mutex _mtx_node_of_name;
            mutable std::mutex _mtx_name_of_node;
            mutable std::mutex _mtx_print;

            int _format_fact_level{0}; // recursion level of method format_fact
        };
    }
}
