#pragma once

namespace zelph
{
    namespace network
    {
        class NodeView
        {
        private:
            const std::map<std::string, std::map<Node, std::wstring>>& _map_ref;

        public:
            explicit NodeView(const std::map<std::string, std::map<Node, std::wstring>>& map_ref)
                : _map_ref(map_ref) {}

            class iterator
            {
            private:
                std::string                                                                  _current_lang;
                typename std::map<Node, std::wstring>::const_iterator                        _inner_it;
                const std::map<std::string, std::map<Node, std::wstring>>*                   _map_ptr;
                typename std::map<std::string, std::map<Node, std::wstring>>::const_iterator _outer_it;
                bool                                                                         _is_end;

                void find_next_valid()
                {
                    // If we're at the end of an inner map, move to the next outer map
                    while (_outer_it != _map_ptr->end() && _inner_it == _outer_it->second.end())
                    {
                        ++_outer_it;
                        if (_outer_it != _map_ptr->end())
                        {
                            _current_lang = _outer_it->first;
                            _inner_it     = _outer_it->second.begin();
                        }
                    }

                    // If we've reached the end of all maps, mark as end
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

                iterator(const std::map<std::string, std::map<Node, std::wstring>>* map_ptr, bool is_end)
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
