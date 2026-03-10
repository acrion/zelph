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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace zelph::network
{
    class adjacency_set
    {
    private:
        struct adj_vec_block
        {
            uint32_t size;
            uint32_t cap;
            Node     data[];

            static adj_vec_block* create(uint32_t capacity)
            {
                auto* block =
                    static_cast<adj_vec_block*>(std::malloc(sizeof(adj_vec_block) + capacity * sizeof(Node)));
                block->size = 0;
                block->cap  = capacity;
                return block;
            }

            static adj_vec_block* create_with(const Node* src, uint32_t count, uint32_t capacity)
            {
                auto* block = create(capacity);
                std::memcpy(block->data, src, count * sizeof(Node));
                block->size = count;
                return block;
            }
        };

        enum class Mode : uint8_t
        {
            Empty  = 0,
            Single = 1,
            Vector = 2
        };

        union Storage
        {
            Node           single_node;
            adj_vec_block* block_ptr;
        };

        Storage _storage{};
        Mode    _mode = Mode::Empty;

        void destroy()
        {
            if (_mode == Mode::Vector)
            {
                std::free(_storage.block_ptr);
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
                _storage.block_ptr = adj_vec_block::create_with(src->data, src->size, src->cap);
                break;
            }
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

            auto* block = adj_vec_block::create(static_cast<uint32_t>(init.size()));
            for (Node n : init)
                block->data[block->size++] = n;
            std::sort(block->data, block->data + block->size);

            // Deduplicate
            uint32_t write = 0;
            for (uint32_t read = 0; read < block->size; ++read)
            {
                if (read == 0 || block->data[read] != block->data[read - 1])
                    block->data[write++] = block->data[read];
            }
            block->size = write;

            if (block->size == 1)
            {
                Node survivor = block->data[0];
                std::free(block);
                _storage.single_node = survivor;
                _mode                = Mode::Single;
            }
            else
            {
                _storage.block_ptr = block;
                _mode              = Mode::Vector;
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
                return _storage.block_ptr->size;
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
            {
                auto* b = _storage.block_ptr;
                return std::binary_search(b->data, b->data + b->size, n) ? 1 : 0;
            }
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
                    auto* block = adj_vec_block::create(2);
                    if (_storage.single_node < n)
                    {
                        block->data[0] = _storage.single_node;
                        block->data[1] = n;
                    }
                    else
                    {
                        block->data[0] = n;
                        block->data[1] = _storage.single_node;
                    }
                    block->size        = 2;
                    _storage.block_ptr = block;
                    _mode              = Mode::Vector;
                }
                break;

            case Mode::Vector:
            {
                auto* b   = _storage.block_ptr;
                Node* pos = std::lower_bound(b->data, b->data + b->size, n);
                if (pos != b->data + b->size && *pos == n) return;

                uint32_t idx = static_cast<uint32_t>(pos - b->data);

                if (b->size == b->cap)
                {
                    uint32_t new_cap   = b->cap * 2;
                    auto*    new_block = adj_vec_block::create(new_cap);
                    std::memcpy(new_block->data, b->data, idx * sizeof(Node));
                    new_block->data[idx] = n;
                    std::memcpy(new_block->data + idx + 1, b->data + idx, (b->size - idx) * sizeof(Node));
                    new_block->size = b->size + 1;
                    std::free(b);
                    _storage.block_ptr = new_block;
                }
                else
                {
                    std::memmove(b->data + idx + 1, b->data + idx, (b->size - idx) * sizeof(Node));
                    b->data[idx] = n;
                    b->size++;
                }
                break;
            }
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
                auto* b   = _storage.block_ptr;
                Node* pos = std::lower_bound(b->data, b->data + b->size, n);
                if (pos != b->data + b->size && *pos == n)
                {
                    uint32_t idx = static_cast<uint32_t>(pos - b->data);
                    b->size--;
                    std::memmove(b->data + idx, b->data + idx + 1, (b->size - idx) * sizeof(Node));

                    if (b->size == 1)
                    {
                        Node survivor = b->data[0];
                        std::free(b);
                        _storage.single_node = survivor;
                        _mode                = Mode::Single;
                    }
                    else if (b->size == 0)
                    {
                        std::free(b);
                        _storage.single_node = 0;
                        _mode                = Mode::Empty;
                    }
                }
                break;
            }
            }
        }

        void clear()
        {
            destroy();
        }

        using const_iterator = const Node*;
        using iterator       = const Node*;

        const_iterator begin() const
        {
            switch (_mode)
            {
            case Mode::Empty:
                return nullptr;
            case Mode::Single:
                return &_storage.single_node;
            case Mode::Vector:
                return _storage.block_ptr->data;
            }
            return nullptr;
        }

        const_iterator end() const
        {
            switch (_mode)
            {
            case Mode::Empty:
                return nullptr;
            case Mode::Single:
                return &_storage.single_node + 1;
            case Mode::Vector:
                return _storage.block_ptr->data + _storage.block_ptr->size;
            }
            return nullptr;
        }

        iterator begin() { return static_cast<const adjacency_set*>(this)->begin(); }
        iterator end() { return static_cast<const adjacency_set*>(this)->end(); }
    };
}