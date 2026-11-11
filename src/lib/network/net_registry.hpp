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

#include "neural.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace zelph::network
{
    // The networks that have been compiled by a single owner, identified
    // through their handle.
    //
    // On each evaluation, from any thread, a handle is retrieved via lookup,
    // so the lookup acquires no lock and writes nothing -- neither reader
    // count nor reference count. Two characteristics enable this behaviour.
    // The registry only grows, and once a slot is written, it remains fixed
    // in place, allowing a reader to access it concurrently while the owner
    // adds new entries. Additionally, a net is never freed while the owner
    // exists: when a load replaces the graph, it retires each handle by
    // clearing its slot, yet retains the net itself, as another thread might
    // still be in the middle of evaluating it using a pointer obtained just
    // prior. The memory occupied by a retired net is only reclaimed when the
    // registry is ultimately destroyed.
    //
    // Handles are never repurposed, meaning a handle acquired before a load
    // is rejected afterwards instead of being assigned to a different net.
    class NetRegistry
    {
    public:
        static constexpr std::size_t chunk_bits = 10;
        static constexpr std::size_t chunk_size = std::size_t{1} << chunk_bits;
        static constexpr std::size_t max_chunks = 2048;
        static constexpr std::size_t capacity   = chunk_size * max_chunks;

        NetRegistry() = default;

        NetRegistry(const NetRegistry&)            = delete;
        NetRegistry& operator=(const NetRegistry&) = delete;

        // Only the owner's thread may call this. An exception is
        // thrown if the registry is full.
        std::int32_t add(std::unique_ptr<NeuralNet> net)
        {
            const std::size_t handle = _count.load(std::memory_order_relaxed);
            if (handle >= capacity || handle > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            {
                throw std::length_error("too many compiled networks: the limit is " + std::to_string(capacity));
            }

            std::atomic<NeuralNet*>* chunk = _chunks[handle >> chunk_bits].load(std::memory_order_relaxed);
            if (!chunk)
            {
                auto block = std::make_unique<std::atomic<NeuralNet*>[]>(chunk_size);
                chunk      = block.get();
                _storage.push_back(std::move(block));
                _chunks[handle >> chunk_bits].store(chunk, std::memory_order_release);
            }

            NeuralNet* const raw = net.get();
            _nets.push_back(std::move(net));
            chunk[handle & (chunk_size - 1)].store(raw, std::memory_order_release);
            // Published last: a reader observing the new count sees the
            // chunk, the slot, and the net built behind it.
            _count.store(handle + 1, std::memory_order_release);
            return static_cast<std::int32_t>(handle);
        }

        // Only the owner's thread. All handles acquired up to this point are
        // now rejected; the nets stay alive (refer to the class comment).
        void retire_all() noexcept
        {
            const std::size_t count = _count.load(std::memory_order_relaxed);
            for (std::size_t handle = 0; handle < count; ++handle)
            {
                _chunks[handle >> chunk_bits]
                    .load(std::memory_order_relaxed)[handle & (chunk_size - 1)]
                    .store(nullptr, std::memory_order_release);
            }
        }

        // Any thread. Null when a handle has never been
        // distributed or has been retired.
        NeuralNet* find(const std::int32_t handle) const noexcept
        {
            if (handle < 0) return nullptr;
            const auto at = static_cast<std::size_t>(handle);
            if (at >= _count.load(std::memory_order_acquire)) return nullptr;
            return _chunks[at >> chunk_bits]
                .load(std::memory_order_acquire)[at & (chunk_size - 1)]
                .load(std::memory_order_acquire);
        }

    private:
        std::array<std::atomic<std::atomic<NeuralNet*>*>, max_chunks> _chunks{};
        std::atomic<std::size_t>                                     _count{0};

        // The owner's alone: the storage located behind the chunks, and
        // each net ever added, retired or not.
        std::vector<std::unique_ptr<std::atomic<NeuralNet*>[]>> _storage;
        std::vector<std::unique_ptr<NeuralNet>>                  _nets;
    };
}
