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

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <type_traits>

namespace zelph::network
{
    class adjacency_set
    {
    private:
        static_assert(std::is_trivially_copyable_v<Node>,
                      "adjacency_set assumes Node is trivially copyable");

        using set_type = ankerl::unordered_dense::set<Node>;

        // Tuning knobs
        static constexpr uint32_t VECTOR_TO_SET_THRESHOLD = 128;
        static constexpr uint32_t SET_TO_VECTOR_THRESHOLD = VECTOR_TO_SET_THRESHOLD / 2;
        static constexpr uint32_t VECTOR_SHRINK_MIN_CAP   = 32;

        struct alignas(Node) adj_vec_block
        {
            uint32_t size;
            uint32_t cap;

            Node* data() noexcept
            {
                return reinterpret_cast<Node*>(this + 1);
            }

            const Node* data() const noexcept
            {
                return reinterpret_cast<const Node*>(this + 1);
            }

            static adj_vec_block* create(uint32_t capacity)
            {
                auto* block = static_cast<adj_vec_block*>(
                    std::malloc(sizeof(adj_vec_block) + capacity * sizeof(Node)));
                if (block == nullptr) throw std::bad_alloc();
                block->size = 0;
                block->cap  = capacity;
                return block;
            }

            static adj_vec_block* create_with(const Node* src, uint32_t count, uint32_t capacity)
            {
                auto* block = create(capacity);
                if (count != 0)
                {
                    std::memcpy(block->data(), src, count * sizeof(Node));
                }
                block->size = count;
                return block;
            }
        };

        enum class Mode : uint8_t
        {
            Empty  = 0,
            Single = 1,
            Vector = 2,
            Set    = 3
        };

        union Storage
        {
            Node           single_node;
            adj_vec_block* block_ptr;
            set_type*      set_ptr;

            Storage() : single_node(0) {}
        };

        Storage _storage{};
        Mode    _mode = Mode::Empty;

        static uint32_t next_growth_capacity(uint32_t current) noexcept
        {
            if (current >= VECTOR_TO_SET_THRESHOLD) return VECTOR_TO_SET_THRESHOLD;
            const uint32_t doubled = current < 2 ? 2u : current * 2u;
            return doubled > VECTOR_TO_SET_THRESHOLD ? VECTOR_TO_SET_THRESHOLD : doubled;
        }

        static uint32_t compact_capacity(uint32_t size) noexcept
        {
            uint32_t cap = 2;
            while (cap < size && cap < VECTOR_TO_SET_THRESHOLD)
            {
                cap *= 2;
            }
            return cap;
        }

        static void sort_and_unique(adj_vec_block* block) noexcept
        {
            Node* const first = block->data();
            Node* const last  = first + block->size;
            std::sort(first, last);
            Node* unique_last = std::unique(first, last);
            block->size       = static_cast<uint32_t>(unique_last - first);
        }

        static adj_vec_block* make_block_from_set(const set_type& set, uint32_t capacity)
        {
            auto* block = adj_vec_block::create(capacity);
            for (Node n : set)
            {
                block->data()[block->size++] = n;
            }
            std::sort(block->data(), block->data() + block->size);
            return block;
        }

        void destroy() noexcept
        {
            if (_mode == Mode::Vector)
            {
                std::free(_storage.block_ptr);
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
            {
                auto* src          = other._storage.block_ptr;
                _storage.block_ptr = adj_vec_block::create_with(src->data(), src->size, src->cap);
                break;
            }

            case Mode::Set:
                _storage.set_ptr = new set_type(*other._storage.set_ptr);
                break;
            }
        }

        void compact_constructed_set(set_type* set)
        {
            const auto sz = static_cast<uint32_t>(set->size());

            if (sz == 0)
            {
                delete set;
                _mode                = Mode::Empty;
                _storage.single_node = 0;
            }
            else if (sz == 1)
            {
                const Node only = *set->begin();
                delete set;
                _mode                = Mode::Single;
                _storage.single_node = only;
            }
            else if (sz <= VECTOR_TO_SET_THRESHOLD)
            {
                auto* block = make_block_from_set(*set, sz);
                delete set;
                _mode              = Mode::Vector;
                _storage.block_ptr = block;
            }
            else
            {
                _mode            = Mode::Set;
                _storage.set_ptr = set;
            }
        }

        void demote_set_if_small()
        {
            auto*      set = _storage.set_ptr;
            const auto sz  = static_cast<uint32_t>(set->size());

            if (sz == 0)
            {
                delete set;
                _mode                = Mode::Empty;
                _storage.single_node = 0;
            }
            else if (sz == 1)
            {
                const Node only = *set->begin();
                delete set;
                _mode                = Mode::Single;
                _storage.single_node = only;
            }
            else if (sz <= SET_TO_VECTOR_THRESHOLD)
            {
                auto* block = make_block_from_set(*set, sz);
                delete set;
                _mode              = Mode::Vector;
                _storage.block_ptr = block;
            }
        }

        void promote_vector_to_set_and_insert(Node n)
        {
            auto* block   = _storage.block_ptr;
            auto* new_set = new set_type();
            new_set->reserve(block->size + 1);
            new_set->insert(block->data(), block->data() + block->size);
            new_set->insert(n);

            std::free(block);
            _mode            = Mode::Set;
            _storage.set_ptr = new_set;
        }

        void maybe_shrink_vector()
        {
            auto* block = _storage.block_ptr;

            if (block->cap < VECTOR_SHRINK_MIN_CAP) return;
            if (block->size > (block->cap / 4)) return;

            const uint32_t new_cap = compact_capacity(block->size);
            if (new_cap >= block->cap) return;

            auto* smaller = adj_vec_block::create_with(block->data(), block->size, new_cap);
            std::free(block);
            _storage.block_ptr = smaller;
        }

        void init_from_small_range(std::initializer_list<Node> init)
        {
            auto* block = adj_vec_block::create(static_cast<uint32_t>(init.size()));

            for (Node n : init)
            {
                if (n != 0)
                {
                    block->data()[block->size++] = n;
                }
            }

            if (block->size == 0)
            {
                std::free(block);
                return;
            }

            sort_and_unique(block);

            if (block->size == 1)
            {
                const Node only = block->data()[0];
                std::free(block);
                _mode                = Mode::Single;
                _storage.single_node = only;
                return;
            }

            // Constructor path: compact to exact size after dedup.
            if (block->size != block->cap)
            {
                auto* compact = adj_vec_block::create_with(block->data(), block->size, block->size);
                std::free(block);
                block = compact;
            }

            _mode              = Mode::Vector;
            _storage.block_ptr = block;
        }

    public:
        adjacency_set() noexcept = default;

        adjacency_set(std::initializer_list<Node> init)
            : adjacency_set()
        {
            if (init.size() == 0) return;

            if (init.size() <= VECTOR_TO_SET_THRESHOLD)
            {
                init_from_small_range(init);
            }
            else
            {
                auto* set = new set_type();
                set->reserve(init.size());

                for (Node n : init)
                {
                    if (n != 0) set->insert(n);
                }

                compact_constructed_set(set);
            }
        }

        ~adjacency_set() { destroy(); }

        adjacency_set(const adjacency_set& other)
        {
            copy_from(other);
        }

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
            : _storage(other._storage), _mode(other._mode)
        {
            other._mode                = Mode::Empty;
            other._storage.single_node = 0;
        }

        adjacency_set& operator=(adjacency_set&& other) noexcept
        {
            if (this != &other)
            {
                destroy();
                _storage = other._storage;
                _mode    = other._mode;

                other._mode                = Mode::Empty;
                other._storage.single_node = 0;
            }
            return *this;
        }

        bool empty() const noexcept
        {
            return _mode == Mode::Empty;
        }

        size_t size() const noexcept
        {
            switch (_mode)
            {
            case Mode::Empty:
                return 0;
            case Mode::Single:
                return 1;
            case Mode::Vector:
                return _storage.block_ptr->size;
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
                return _storage.single_node == n ? 1u : 0u;

            case Mode::Vector:
            {
                auto* block = _storage.block_ptr;
                return std::binary_search(block->data(), block->data() + block->size, n) ? 1u : 0u;
            }

            case Mode::Set:
                return _storage.set_ptr->count(n);
            }

            return 0;
        }

        bool contains(Node n) const
        {
            return count(n) != 0;
        }

        void insert(Node n)
        {
            if (n == 0) return; // invalid sentinel

            switch (_mode)
            {
            case Mode::Empty:
                _mode                = Mode::Single;
                _storage.single_node = n;
                return;

            case Mode::Single:
                if (_storage.single_node == n) return;
                {
                    auto* block = adj_vec_block::create(2);
                    if (_storage.single_node < n)
                    {
                        block->data()[0] = _storage.single_node;
                        block->data()[1] = n;
                    }
                    else
                    {
                        block->data()[0] = n;
                        block->data()[1] = _storage.single_node;
                    }
                    block->size        = 2;
                    _mode              = Mode::Vector;
                    _storage.block_ptr = block;
                }
                return;

            case Mode::Vector:
            {
                auto* block = _storage.block_ptr;
                Node* data  = block->data();

                // Very cheap fast path for append-heavy workloads.
                if (n == data[block->size - 1]) return;

                if (n > data[block->size - 1])
                {
                    if (block->size == VECTOR_TO_SET_THRESHOLD)
                    {
                        promote_vector_to_set_and_insert(n);
                        return;
                    }

                    if (block->size == block->cap)
                    {
                        const uint32_t new_cap = next_growth_capacity(block->cap);
                        auto*          bigger  = adj_vec_block::create_with(block->data(), block->size, new_cap);
                        std::free(block);
                        block              = bigger;
                        _storage.block_ptr = bigger;
                        data               = block->data();
                    }

                    data[block->size++] = n;
                    return;
                }

                Node* pos = std::lower_bound(data, data + block->size, n);
                if (pos != data + block->size && *pos == n) return;

                if (block->size == VECTOR_TO_SET_THRESHOLD)
                {
                    promote_vector_to_set_and_insert(n);
                    return;
                }

                const uint32_t idx = static_cast<uint32_t>(pos - data);

                if (block->size == block->cap)
                {
                    const uint32_t new_cap = next_growth_capacity(block->cap);
                    auto*          bigger  = adj_vec_block::create(new_cap);

                    std::memcpy(bigger->data(), data, idx * sizeof(Node));
                    bigger->data()[idx] = n;
                    std::memcpy(bigger->data() + idx + 1, data + idx, (block->size - idx) * sizeof(Node));
                    bigger->size = block->size + 1;

                    std::free(block);
                    _storage.block_ptr = bigger;
                }
                else
                {
                    std::memmove(data + idx + 1, data + idx, (block->size - idx) * sizeof(Node));
                    data[idx] = n;
                    ++block->size;
                }

                return;
            }

            case Mode::Set:
                _storage.set_ptr->insert(n);
                return;
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
                return;

            case Mode::Vector:
            {
                auto* block = _storage.block_ptr;
                Node* data  = block->data();
                Node* pos   = std::lower_bound(data, data + block->size, n);

                if (pos == data + block->size || *pos != n) return;

                const uint32_t idx = static_cast<uint32_t>(pos - data);
                --block->size;
                std::memmove(data + idx, data + idx + 1, (block->size - idx) * sizeof(Node));

                if (block->size == 1)
                {
                    const Node only = data[0];
                    std::free(block);
                    _mode                = Mode::Single;
                    _storage.single_node = only;
                }
                else if (block->size == 0)
                {
                    std::free(block);
                    _mode                = Mode::Empty;
                    _storage.single_node = 0;
                }
                else
                {
                    maybe_shrink_vector();
                }

                return;
            }

            case Mode::Set:
                if (_storage.set_ptr->erase(n) != 0)
                {
                    demote_set_if_small();
                }
                return;
            }
        }

        void clear() noexcept
        {
            destroy();
        }

        class const_iterator
        {
        private:
            enum class Kind : uint8_t
            {
                Empty,
                PointerRange,
                Set
            };

            using SetIter = typename set_type::const_iterator;

            Kind        _kind = Kind::Empty;
            const Node* _ptr  = nullptr;
            const Node* _end  = nullptr;

            alignas(SetIter) unsigned char _it_buf[sizeof(SetIter)];
            alignas(SetIter) unsigned char _end_buf[sizeof(SetIter)];

            SetIter& set_it() noexcept
            {
                return *std::launder(reinterpret_cast<SetIter*>(_it_buf));
            }

            const SetIter& set_it() const noexcept
            {
                return *std::launder(reinterpret_cast<const SetIter*>(_it_buf));
            }

            SetIter& set_end() noexcept
            {
                return *std::launder(reinterpret_cast<SetIter*>(_end_buf));
            }

            const SetIter& set_end() const noexcept
            {
                return *std::launder(reinterpret_cast<const SetIter*>(_end_buf));
            }

            void destroy() noexcept
            {
                if (_kind == Kind::Set)
                {
                    set_it().~SetIter();
                    set_end().~SetIter();
                }
                _kind = Kind::Empty;
                _ptr  = nullptr;
                _end  = nullptr;
            }

        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type        = Node;
            using difference_type   = std::ptrdiff_t;
            using pointer           = const Node*;
            using reference         = const Node&;

            const_iterator() noexcept = default;

            const_iterator(const Node* ptr, const Node* end) noexcept
                : _kind(ptr == nullptr ? Kind::Empty : Kind::PointerRange), _ptr(ptr), _end(end)
            {
            }

            const_iterator(SetIter it, SetIter end)
                : _kind(Kind::Set)
            {
                new (_it_buf) SetIter(std::move(it));
                new (_end_buf) SetIter(std::move(end));
            }

            ~const_iterator()
            {
                destroy();
            }

            const_iterator(const const_iterator& other)
                : _kind(other._kind), _ptr(other._ptr), _end(other._end)
            {
                if (_kind == Kind::Set)
                {
                    new (_it_buf) SetIter(other.set_it());
                    new (_end_buf) SetIter(other.set_end());
                }
            }

            const_iterator(const_iterator&& other) noexcept
                : _kind(other._kind), _ptr(other._ptr), _end(other._end)
            {
                if (_kind == Kind::Set)
                {
                    new (_it_buf) SetIter(other.set_it());
                    new (_end_buf) SetIter(other.set_end());
                }
            }

            const_iterator& operator=(const const_iterator& other)
            {
                if (this != &other)
                {
                    destroy();
                    _kind = other._kind;
                    _ptr  = other._ptr;
                    _end  = other._end;

                    if (_kind == Kind::Set)
                    {
                        new (_it_buf) SetIter(other.set_it());
                        new (_end_buf) SetIter(other.set_end());
                    }
                }
                return *this;
            }

            const_iterator& operator=(const_iterator&& other) noexcept
            {
                if (this != &other)
                {
                    destroy();
                    _kind = other._kind;
                    _ptr  = other._ptr;
                    _end  = other._end;

                    if (_kind == Kind::Set)
                    {
                        new (_it_buf) SetIter(other.set_it());
                        new (_end_buf) SetIter(other.set_end());
                    }
                }
                return *this;
            }

            reference operator*() const noexcept
            {
                return _kind == Kind::Set ? *set_it() : *_ptr;
            }

            pointer operator->() const noexcept
            {
                return _kind == Kind::Set ? &(*set_it()) : _ptr;
            }

            const_iterator& operator++() noexcept
            {
                if (_kind == Kind::PointerRange)
                {
                    ++_ptr;
                }
                else if (_kind == Kind::Set)
                {
                    ++set_it();
                }
                return *this;
            }

            const_iterator operator++(int) noexcept
            {
                const_iterator tmp(*this);
                ++(*this);
                return tmp;
            }

            bool operator==(const const_iterator& other) const noexcept
            {
                if (_kind != other._kind) return false;

                switch (_kind)
                {
                case Kind::Empty:
                    return true;
                case Kind::PointerRange:
                    return _ptr == other._ptr && _end == other._end;
                case Kind::Set:
                    return set_it() == other.set_it();
                }

                return false;
            }

            bool operator!=(const const_iterator& other) const noexcept
            {
                return !(*this == other);
            }
        };

        using iterator = const_iterator;

        const_iterator begin() const
        {
            switch (_mode)
            {
            case Mode::Empty:
                return {};
            case Mode::Single:
                return {&_storage.single_node, &_storage.single_node + 1};
            case Mode::Vector:
                return {_storage.block_ptr->data(),
                        _storage.block_ptr->data() + _storage.block_ptr->size};
            case Mode::Set:
                return {_storage.set_ptr->begin(), _storage.set_ptr->end()};
            }
            return {};
        }

        const_iterator end() const
        {
            switch (_mode)
            {
            case Mode::Empty:
                return {};
            case Mode::Single:
                return {&_storage.single_node + 1, &_storage.single_node + 1};
            case Mode::Vector:
                return {_storage.block_ptr->data() + _storage.block_ptr->size,
                        _storage.block_ptr->data() + _storage.block_ptr->size};
            case Mode::Set:
                return {_storage.set_ptr->end(), _storage.set_ptr->end()};
            }
            return {};
        }

        iterator begin()
        {
            return static_cast<const adjacency_set*>(this)->begin();
        }

        iterator end()
        {
            return static_cast<const adjacency_set*>(this)->end();
        }
    };
}