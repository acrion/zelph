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

#include <ankerl/unordered_dense.h>

#include "network_types.hpp"

namespace zelph
{
    namespace network
    {
        class NodeView
        {
        private:
            const ankerl::unordered_dense::map<std::string,
                                               ankerl::unordered_dense::map<Node, std::wstring>>& _map_ref;

        public:
            explicit NodeView(const ankerl::unordered_dense::map<std::string,
                                                                 ankerl::unordered_dense::map<Node, std::wstring>>& map_ref)
                : _map_ref(map_ref) {}

            class iterator
            {
            private:
                std::string                                                                                    _current_lang;
                ankerl::unordered_dense::map<Node, std::wstring>::const_iterator                               _inner_it;
                const ankerl::unordered_dense::map<std::string,
                                                   ankerl::unordered_dense::map<Node, std::wstring>>*          _map_ptr;
                ankerl::unordered_dense::map<std::string,
                                             ankerl::unordered_dense::map<Node, std::wstring>>::const_iterator _outer_it;
                bool                                                                                           _is_end;

                void find_next_valid()
                {
                    while (_outer_it != _map_ptr->end() && _inner_it == _outer_it->second.end())
                    {
                        ++_outer_it;
                        if (_outer_it != _map_ptr->end())
                        {
                            _current_lang = _outer_it->first;
                            _inner_it     = _outer_it->second.begin();
                        }
                    }

                    if (_outer_it == _map_ptr->end())
                    {
                        _is_end = true;
                    }
                }

            public:
                using iterator_category = std::forward_iterator_tag;
                using value_type        = Node;
                using difference_type   = std::ptrdiff_t;
                using pointer           = const Node*;
                using reference         = const Node&;

                iterator(const ankerl::unordered_dense::map<std::string,
                                                            ankerl::unordered_dense::map<Node, std::wstring>>* map_ptr,
                         bool                                                                                  is_end)
                    : _map_ptr(map_ptr), _is_end(is_end)
                {
                    if (!is_end && !map_ptr->empty())
                    {
                        _outer_it     = map_ptr->begin();
                        _current_lang = _outer_it->first;
                        _inner_it     = _outer_it->second.begin();
                        find_next_valid();
                    }
                }

                bool operator==(const iterator& other) const
                {
                    if (_is_end && other._is_end) return true;
                    if (_is_end || other._is_end) return false;
                    return _outer_it == other._outer_it && _inner_it == other._inner_it;
                }

                bool operator!=(const iterator& other) const
                {
                    return !(*this == other);
                }

                reference operator*() const
                {
                    return _inner_it->first;
                }

                pointer operator->() const
                {
                    return &(_inner_it->first);
                }

                iterator& operator++()
                {
                    if (!_is_end)
                    {
                        ++_inner_it;
                        find_next_valid();
                    }
                    return *this;
                }

                iterator operator++(int)
                {
                    iterator tmp = *this;
                    ++(*this);
                    return tmp;
                }
            };

            iterator begin() const
            {
                return iterator(&_map_ref, false);
            }

            iterator end() const
            {
                return iterator(&_map_ref, true);
            }
        };
    }
}