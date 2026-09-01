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

// Reading a serialized .bin WITHOUT loading the network it holds.
//
// This is what ".stat-file" and ".index-file" are made of, and it is not
// command logic: no graph, no REPL state, no dot-command vocabulary -- a file
// name goes in and a description of the file comes out. It lived inside the
// command executor because that is where its two callers are, and it brought
// Cap'n Proto and kj into a translation unit that is otherwise about typed
// lines.

#include <cstdint>
#include <string>
#include <vector>

namespace zelph::io
{
    struct BinHeaderStats
    {
        uint32_t left_chunk_count    = 0;
        uint32_t right_chunk_count   = 0;
        uint32_t name_of_node_count  = 0;
        uint32_t node_of_name_count  = 0;
        uint64_t file_size_bytes     = 0;
        uint64_t header_length_bytes = 0;
    };

    // Smallest number of bytes a chunk message can occupy. A packed Cap'n
    // Proto message begins with its segment table, so nothing below this is
    // possible; only used to reject a header that declares more chunks than
    // the file could ever hold.
    static constexpr uint64_t kMinimumChunkBytes = 8;

    struct BinChunkRef
    {
        uint32_t    chunk_index = 0;
        uint64_t    offset      = 0;
        uint64_t    length      = 0;
        std::string which;
        std::string lang;
    };

    struct BinIndexData
    {
        std::string              filename;
        BinHeaderStats           stats;
        uint64_t                 header_length_bytes = 0;
        std::vector<BinChunkRef> left_chunks;
        std::vector<BinChunkRef> right_chunks;
        std::vector<BinChunkRef> name_of_node_chunks;
        std::vector<BinChunkRef> node_of_name_chunks;
    };

    /// The header only, which is what makes this instant on an 88 GB file.
    /// Throws with the file named -- and with the command that asked -- when
    /// it is not a readable zelph .bin.
    BinHeaderStats read_bin_header_stats(const std::string& command, const std::string& filename);

    /// Walk every chunk and record where it begins and how long it is.
    BinIndexData read_bin_index_data(const std::string& command, const std::string& filename);

    /// Write what read_bin_index_data produced, as JSON.
    void write_bin_index_json(const BinIndexData& data, const std::string& output_filename);
}
