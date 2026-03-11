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

#include <filesystem>
#include <functional>
#include <string>

namespace zelph::io
{
    using DiagnosticCallback = std::function<void(const std::string&)>;

    class ReadAsync
    {
    public:
        explicit ReadAsync(const std::filesystem::path& file_name, size_t sufficient_batch_count = 2, DiagnosticCallback diagnostic_callback = {});
        ~ReadAsync();
        ReadAsync(const ReadAsync&)            = delete;
        ReadAsync& operator=(const ReadAsync&) = delete;
        ReadAsync(ReadAsync&&)                 = delete;
        ReadAsync& operator=(ReadAsync&&)      = delete;

        std::streamsize get_total_size() const;

        // Returns wide string (performs conversion)
        bool get_line(std::string& line, std::streamoff& streampos) const;

        // Returns raw UTF-8 string (no conversion)
        bool get_line_utf8(std::string& line, std::streamoff& streampos) const;

        // Thread-safe batch retrieval: each caller gets a full batch without external locking.
        // Returns false when EOF is reached and no more batches are available.
        bool get_batch(std::vector<std::pair<std::string, std::streamoff>>& lines) const;

        std::string error_text() const;

        struct StatsSnapshot
        {
            uint64_t    batches_enqueued{0};
            uint64_t    entries_enqueued{0};
            uint64_t    source_bytes_read{0};
            uint64_t    output_bytes_emitted{0};
            uint64_t    queue_wait_not_full_ns{0};
            uint64_t    max_queue_size{0};
            size_t      queue_capacity{0};
            bool        compressed{false};
            bool        using_external_decompressor{false};
            std::string decompressor_name;
        };

        StatsSnapshot get_stats_snapshot() const;

        class Impl;
        Impl* const _pImpl; // must stay at top of members list because of initialization order
    };
}