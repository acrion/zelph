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

#include <ankerl/unordered_dense.h>

#include <boost/serialization/vector.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace zelph
{
    namespace network
    {
        class ZELPH_EXPORT Zelph::Impl : public Network
        {
            friend class Zelph;
            Impl() = default;

            friend class boost::serialization::access;

            template <class Archive>
            void serialize(Archive& ar, const unsigned int version)
            {
                ar& boost::serialization::base_object<Network>(*this);

                std::size_t outer_size = _name_of_node.size();
                ar & outer_size;
                if constexpr (Archive::is_loading::value)
                {
                    _name_of_node.clear();
                    for (std::size_t i = 0; i < outer_size; ++i)
                    {
                        std::string lang;
                        ar & lang;
                        name_of_node_map inner;
                        std::size_t      inner_size;
                        ar & inner_size;
                        inner.reserve(inner_size);
                        for (std::size_t j = 0; j < inner_size; ++j)
                        {
                            Node         key;
                            std::wstring value;
                            ar & key;
                            ar & value;
                            inner[key] = value;
                        }
                        _name_of_node[lang] = std::move(inner);
                    }
                }
                else
                {
                    for (const auto& p : _name_of_node)
                    {
                        ar & p.first;
                        std::size_t inner_size = p.second.size();
                        ar & inner_size;
                        for (const auto& ip : p.second)
                        {
                            ar & ip.first;
                            ar & ip.second;
                        }
                    }
                }

                outer_size = _node_of_name.size();
                ar & outer_size;
                if constexpr (Archive::is_loading::value)
                {
                    _node_of_name.clear();
                    for (std::size_t i = 0; i < outer_size; ++i)
                    {
                        std::string lang;
                        ar & lang;
                        node_of_name_map inner;
                        std::size_t      inner_size;
                        ar & inner_size;
                        inner.reserve(inner_size);
                        for (std::size_t j = 0; j < inner_size; ++j)
                        {
                            std::wstring key;
                            Node         value;
                            ar & key;
                            ar & value;
                            inner[key] = value;
                        }
                        _node_of_name[lang] = std::move(inner);
                    }
                }
                else
                {
                    for (const auto& p : _node_of_name)
                    {
                        ar & p.first;
                        std::size_t inner_size = p.second.size();
                        ar & inner_size;
                        for (const auto& ip : p.second)
                        {
                            ar & ip.first;
                            ar & ip.second;
                        }
                    }
                }
            }

            using name_of_node_map = ankerl::unordered_dense::map<Node, std::wstring>;
            using node_of_name_map = ankerl::unordered_dense::map<std::wstring, Node>;

            ankerl::unordered_dense::map<std::string, name_of_node_map> _name_of_node;
            ankerl::unordered_dense::map<std::string, node_of_name_map> _node_of_name;

            mutable std::mutex _mtx_node_of_name;
            mutable std::mutex _mtx_name_of_node;
            mutable std::mutex _mtx_print;

            int _format_fact_level{0}; // recursion level of method format_fact
        };
    }
}