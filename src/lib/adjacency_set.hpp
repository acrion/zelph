#pragma once

#include "network_types.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <variant>
#include <vector>

namespace zelph
{
    namespace network
    {
        class adjacency_set
        {
        private:
            static constexpr size_t VECTOR_TO_SET_THRESHOLD = 128;

            enum class Mode : uint8_t
            {
                Empty  = 0,
                Single = 1,
                Vector = 2,
                Set    = 3
            };

            union Storage
            {
                Node                                single_node;
                std::vector<Node>*                  vec_ptr;
                ankerl::unordered_dense::set<Node>* set_ptr;
            };

            Storage _storage;
            Mode    _mode = Mode::Empty;

            void destroy()
            {
                if (_mode == Mode::Vector)
                {
                    delete _storage.vec_ptr;
                }
                else if (_mode == Mode::Set)
                {
                    delete _storage.set_ptr;
                }
                _mode                = Mode::Empty;
                _storage.single_node = 0;
            }

            void copy_from(const adjacency_set& other)
            {
                _mode = other._mode;
                switch (_mode)
                {
                case Mode::Empty:
                    _storage.single_node = 0;
                    break;
                case Mode::Single:
                    _storage.single_node = other._storage.single_node;
                    break;
                case Mode::Vector:
                    _storage.vec_ptr = new std::vector<Node>(*other._storage.vec_ptr);
                    break;
                case Mode::Set:
                    _storage.set_ptr = new ankerl::unordered_dense::set<Node>(*other._storage.set_ptr);
                    break;
                }
            }

        public:
            adjacency_set()
            {
                _mode                = Mode::Empty;
                _storage.single_node = 0;
            }

            adjacency_set(std::initializer_list<Node> init) : adjacency_set()
            {
                if (init.size() == 0) return;

                if (init.size() == 1)
                {
                    insert(*init.begin());
                    return;
                }

                if (init.size() <= VECTOR_TO_SET_THRESHOLD)
                {
                    _storage.vec_ptr = new std::vector<Node>();
                    _storage.vec_ptr->reserve(init.size());
                    _mode = Mode::Vector;
                    for (Node n : init)
                        _storage.vec_ptr->push_back(n);
                    std::sort(_storage.vec_ptr->begin(), _storage.vec_ptr->end());

                    auto last = std::unique(_storage.vec_ptr->begin(), _storage.vec_ptr->end());
                    _storage.vec_ptr->erase(last, _storage.vec_ptr->end());
                }
                else
                {
                    for (Node n : init)
                        insert(n);
                }
            }

            ~adjacency_set() { destroy(); }

            adjacency_set(const adjacency_set& other) { copy_from(other); }

            adjacency_set& operator=(const adjacency_set& other)
            {
                if (this != &other)
                {
                    destroy();
                    copy_from(other);
                }
                return *this;
            }

            adjacency_set(adjacency_set&& other) noexcept
            {
                _mode                      = other._mode;
                _storage                   = other._storage;
                other._mode                = Mode::Empty;
                other._storage.single_node = 0;
            }

            adjacency_set& operator=(adjacency_set&& other) noexcept
            {
                if (this != &other)
                {
                    destroy();
                    _mode                      = other._mode;
                    _storage                   = other._storage;
                    other._mode                = Mode::Empty;
                    other._storage.single_node = 0;
                }
                return *this;
            }

            bool empty() const
            {
                return _mode == Mode::Empty;
            }

            size_t size() const
            {
                switch (_mode)
                {
                case Mode::Empty:
                    return 0;
                case Mode::Single:
                    return 1;
                case Mode::Vector:
                    return _storage.vec_ptr->size();
                case Mode::Set:
                    return _storage.set_ptr->size();
                }
                return 0;
            }

            size_t count(Node n) const
            {
                switch (_mode)
                {
                case Mode::Empty:
                    return 0;
                case Mode::Single:
                    return (_storage.single_node == n) ? 1 : 0;
                case Mode::Vector:
                    return std::binary_search(_storage.vec_ptr->begin(), _storage.vec_ptr->end(), n) ? 1 : 0;
                case Mode::Set:
                    return _storage.set_ptr->count(n);
                }
                return 0;
            }

            void insert(Node n)
            {
                if (n == 0) return; // Node 0 is invalid/empty

                switch (_mode)
                {
                case Mode::Empty:
                    _mode                = Mode::Single;
                    _storage.single_node = n;
                    break;

                case Mode::Single:
                    if (_storage.single_node == n) return;
                    {
                        auto new_vec = new std::vector<Node>();
                        new_vec->reserve(2);
                        if (_storage.single_node < n)
                        {
                            new_vec->push_back(_storage.single_node);
                            new_vec->push_back(n);
                        }
                        else
                        {
                            new_vec->push_back(n);
                            new_vec->push_back(_storage.single_node);
                        }
                        _storage.vec_ptr = new_vec;
                        _mode            = Mode::Vector;
                    }
                    break;

                case Mode::Vector:
                {
                    auto& vec = *_storage.vec_ptr;
                    auto  it  = std::lower_bound(vec.begin(), vec.end(), n);
                    if (it == vec.end() || *it != n)
                    {
                        vec.insert(it, n);
                        if (vec.size() > VECTOR_TO_SET_THRESHOLD)
                        {
                            auto new_set = new ankerl::unordered_dense::set<Node>(vec.begin(), vec.end());
                            delete _storage.vec_ptr;
                            _storage.set_ptr = new_set;
                            _mode            = Mode::Set;
                        }
                    }
                    break;
                }

                case Mode::Set:
                    _storage.set_ptr->insert(n);
                    break;
                }
            }

            void erase(Node n)
            {
                switch (_mode)
                {
                case Mode::Empty:
                    return;
                case Mode::Single:
                    if (_storage.single_node == n)
                    {
                        _storage.single_node = 0;
                        _mode                = Mode::Empty;
                    }
                    break;
                case Mode::Vector:
                {
                    auto& vec = *_storage.vec_ptr;
                    auto  it  = std::lower_bound(vec.begin(), vec.end(), n);
                    if (it != vec.end() && *it == n)
                    {
                        vec.erase(it);
                        if (vec.size() == 1)
                        {
                            Node survivor = vec[0];
                            delete _storage.vec_ptr;
                            _storage.single_node = survivor;
                            _mode                = Mode::Single;
                        }
                        else if (vec.empty())
                        {
                            delete _storage.vec_ptr;
                            _mode = Mode::Empty;
                        }
                    }
                    break;
                }
                case Mode::Set:
                    _storage.set_ptr->erase(n);
                    if (_storage.set_ptr->size() < (VECTOR_TO_SET_THRESHOLD / 2))
                    {
                        auto new_vec = new std::vector<Node>(_storage.set_ptr->begin(), _storage.set_ptr->end());
                        std::sort(new_vec->begin(), new_vec->end());
                        delete _storage.set_ptr;
                        _storage.vec_ptr = new_vec;
                        _mode            = Mode::Vector;
                    }
                    break;
                }
            }

            void clear()
            {
                destroy();
            }

            class const_iterator
            {
            public:
                using iterator_category = std::forward_iterator_tag;
                using value_type        = Node;
                using difference_type   = std::ptrdiff_t;
                using pointer           = const Node*;
                using reference         = const Node&;

                using VecIter = typename std::vector<Node>::const_iterator;

            private:
                Mode _mode;
                union
                {
                    const Node* ptr_single;
                    VecIter     it_generic;
                };

                bool _single_is_end = false;

            public:
                const_iterator()
                    : _mode(Mode::Empty), ptr_single(nullptr) {}

                explicit const_iterator(std::nullptr_t)
                    : _mode(Mode::Empty), ptr_single(nullptr) {}

                const_iterator(const Node* ptr, bool is_end)
                    : _mode(Mode::Single), ptr_single(ptr), _single_is_end(is_end) {}

                const_iterator(VecIter it, Mode m)
                    : _mode(m)
                {
                    // Konstruiert den Iterator im Speicher der Union
                    new (&it_generic) VecIter(it);
                }

                // 1. Destruktor
                ~const_iterator()
                {
                    if (_mode == Mode::Vector || _mode == Mode::Set)
                    {
                        it_generic.~VecIter(); // Korrekter Aufruf über den Alias
                    }
                }

                // 2. Copy Constructor
                const_iterator(const const_iterator& other)
                    : _mode(other._mode), _single_is_end(other._single_is_end)
                {
                    if (_mode == Mode::Single || _mode == Mode::Empty)
                    {
                        ptr_single = other.ptr_single;
                    }
                    else
                    {
                        new (&it_generic) VecIter(other.it_generic);
                    }
                }

                // 3. Move Constructor
                const_iterator(const_iterator&& other) noexcept
                    : _mode(other._mode), _single_is_end(other._single_is_end)
                {
                    if (_mode == Mode::Single || _mode == Mode::Empty)
                    {
                        ptr_single = other.ptr_single;
                    }
                    else
                    {
                        new (&it_generic) VecIter(std::move(other.it_generic));
                    }
                }

                // 4. Copy Assignment
                const_iterator& operator=(const const_iterator& other)
                {
                    if (this != &other)
                    {
                        // Alten Iterator zerstören, falls aktiv
                        if (_mode == Mode::Vector || _mode == Mode::Set)
                        {
                            it_generic.~VecIter();
                        }

                        _mode          = other._mode;
                        _single_is_end = other._single_is_end;

                        if (_mode == Mode::Single || _mode == Mode::Empty)
                        {
                            ptr_single = other.ptr_single;
                        }
                        else
                        {
                            new (&it_generic) VecIter(other.it_generic);
                        }
                    }
                    return *this;
                }

                // 5. Move Assignment
                const_iterator& operator=(const_iterator&& other) noexcept
                {
                    if (this != &other)
                    {
                        if (_mode == Mode::Vector || _mode == Mode::Set)
                        {
                            it_generic.~VecIter();
                        }

                        _mode          = other._mode;
                        _single_is_end = other._single_is_end;

                        if (_mode == Mode::Single || _mode == Mode::Empty)
                        {
                            ptr_single = other.ptr_single;
                        }
                        else
                        {
                            new (&it_generic) VecIter(std::move(other.it_generic));
                        }
                    }
                    return *this;
                }

                reference operator*() const
                {
                    if (_mode == Mode::Single) return *ptr_single;
                    return *it_generic;
                }

                pointer operator->() const
                {
                    if (_mode == Mode::Single) return ptr_single;
                    return &(*it_generic);
                }

                const_iterator& operator++()
                {
                    switch (_mode)
                    {
                    case Mode::Empty:
                        break;
                    case Mode::Single:
                        _single_is_end = true;
                        break;
                    case Mode::Vector:
                    case Mode::Set:
                        ++it_generic;
                        break;
                    }
                    return *this;
                }

                bool operator!=(const const_iterator& other) const
                {
                    if (_mode != other._mode) return true;
                    switch (_mode)
                    {
                    case Mode::Empty:
                        return false;
                    case Mode::Single:
                        return _single_is_end != other._single_is_end;
                    case Mode::Vector:
                    case Mode::Set:
                        return it_generic != other.it_generic;
                    }
                    return false;
                }

                bool operator==(const const_iterator& other) const
                {
                    return !(*this != other);
                }
            };

            using iterator = const_iterator;

            const_iterator begin() const
            {
                switch (_mode)
                {
                case Mode::Empty:
                    return const_iterator(nullptr);
                case Mode::Single:
                    return const_iterator(&_storage.single_node, false);
                case Mode::Vector:
                    return const_iterator(_storage.vec_ptr->begin(), Mode::Vector);
                case Mode::Set:
                    return const_iterator(_storage.set_ptr->begin(), Mode::Set);
                }
                return const_iterator(nullptr);
            }

            const_iterator end() const
            {
                switch (_mode)
                {
                case Mode::Empty:
                    return const_iterator(nullptr);
                case Mode::Single:
                    return const_iterator(&_storage.single_node, true);
                case Mode::Vector:
                    return const_iterator(_storage.vec_ptr->end(), Mode::Vector);
                case Mode::Set:
                    return const_iterator(_storage.set_ptr->end(), Mode::Set);
                }
                return const_iterator(nullptr);
            }

            iterator begin() { return static_cast<const adjacency_set*>(this)->begin(); }
            iterator end() { return static_cast<const adjacency_set*>(this)->end(); }
        };

    }
}