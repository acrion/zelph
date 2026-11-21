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

// Rewriting the chunk directory of a saved network, for testing scenarios
// involving a directory that does not match its file, and writing a network
// without one, or without the records that every published network lacks.

#include "io/zelph.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/serialize.h>
#include <doctest/doctest.h>
#include <kj/io.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

namespace zelph::test
{
    // Rewrites the chunk directory of the network within `file` using
    // `change`, leaving the header and the chunks unchanged: whatever a
    // damaged file or a writer other than the engine might hold there,
    // written via Cap'n Proto exactly as the engine does. The change
    // modifies only the numbers, and since the directory is unpacked, it
    // keeps its size and the chunks retain their place.
    template <typename Change>
    void rewrite_directory(const std::filesystem::path& file, const Change& change)
    {
        std::vector<kj::byte> bytes;
        {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        kj::ArrayInputStream input(kj::arrayPtr(bytes.data(), bytes.size()));
        {
            ::capnp::PackedMessageReader header(input);
        }
        const auto        behind_header = input.tryGetReadBuffer();
        const std::size_t offset        = bytes.size() - behind_header.size();

        // In words, as Cap'n Proto reads a message: the directory
        // commences at the position where the packed header ends.
        std::vector<std::uint64_t> words(behind_header.size() / sizeof(std::uint64_t));
        std::memcpy(words.data(), behind_header.begin(), words.size() * sizeof(std::uint64_t));
        const kj::ArrayPtr<const ::capnp::word> flat(reinterpret_cast<const ::capnp::word*>(words.data()), words.size());

        ::capnp::MallocMessageBuilder directory;
        const auto                    behind_directory = ::capnp::initMessageBuilderFromFlatArrayCopy(flat, directory);
        const std::size_t             length           = (flat.size() - behind_directory.size()) * sizeof(::capnp::word);
        change(directory.getRoot<zelph::network::ChunkDirectory>());
        const auto written = ::capnp::messageToFlatArray(directory);
        REQUIRE(written.asBytes().size() == length);
        std::memcpy(bytes.data() + offset, written.asBytes().begin(), length);

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // Rewrites the segment table that begins the chunk directory in `file`, so
    // that the directory claims every byte behind the header, just as a single
    // damaged field can: the size of its sole segment. When `claimed_words` is
    // provided, the field claims that many words instead, which may be more
    // than the file holds. All other bytes remain unchanged. Returns the number
    // of bytes the directory claims.
    inline std::uint64_t claim_for_directory(const std::filesystem::path& file, const std::optional<std::uint32_t> claimed_words = std::nullopt)
    {
        std::vector<kj::byte> bytes;
        {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        kj::ArrayInputStream input(kj::arrayPtr(bytes.data(), bytes.size()));
        {
            ::capnp::PackedMessageReader header(input);
        }
        const std::size_t behind_header = input.tryGetReadBuffer().size();
        const std::size_t offset        = bytes.size() - behind_header;

        // The initial word of the table: the count of segments less one,
        // and the size of the first segment measured in words.
        std::uint32_t further_segments = 0;
        REQUIRE(behind_header >= sizeof(::capnp::word));
        std::memcpy(&further_segments, bytes.data() + offset, sizeof(further_segments));
        REQUIRE(further_segments == 0);
        const std::uint32_t words = claimed_words ? *claimed_words : static_cast<std::uint32_t>(behind_header / sizeof(::capnp::word) - 1);
        std::memcpy(bytes.data() + offset + sizeof(further_segments), &words, sizeof(words));

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return (std::uint64_t{words} + 1) * sizeof(::capnp::word);
    }

    // Rewrites the header of the network inside `file` using `change` and
    // leaves the chunk directory and the chunks unchanged: what a damaged
    // header holds, or one originating from a writer other than the engine. The
    // header is packed, so its length is determined by its contents, and the
    // directory following it indicates the location of the chunks only while
    // that length stays the same: the change must preserve it, which a number
    // does that is nonzero before the change and nonzero afterwards.
    template <typename Change>
    void rewrite_header(const std::filesystem::path& file, const Change& change)
    {
        std::vector<kj::byte> bytes;
        {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        kj::ArrayInputStream          input(kj::arrayPtr(bytes.data(), bytes.size()));
        ::capnp::MallocMessageBuilder header;
        {
            ::capnp::PackedMessageReader read(input);
            header.setRoot(read.getRoot<zelph::network::ZelphImpl>());
        }
        const std::size_t length = bytes.size() - input.tryGetReadBuffer().size();
        change(header.getRoot<zelph::network::ZelphImpl>());

        kj::VectorOutputStream written;
        ::capnp::writePackedMessage(written, header);
        REQUIRE(written.getArray().size() == length);
        std::memcpy(bytes.data(), written.getArray().begin(), length);

        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // Writes the network from `from` to `to` in the format of an engine
    // prior to the chunk directory, which every network published before
    // it uses: the header lacking the flag that announces a directory, and
    // the chunks immediately following it. With `without_records`, the
    // header also lacks what those engines did not record either: the
    // template-variable store and the kinds of pattern the network might
    // hold.
    inline void write_without_directory(const std::filesystem::path& from, const std::filesystem::path& to, const bool without_records = false)
    {
        std::vector<kj::byte> bytes;
        {
            std::ifstream in(from, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        kj::ArrayInputStream          input(kj::arrayPtr(bytes.data(), bytes.size()));
        ::capnp::MallocMessageBuilder header;
        {
            ::capnp::PackedMessageReader read(input);
            header.setRoot(read.getRoot<zelph::network::ZelphImpl>());
        }
        header.getRoot<zelph::network::ZelphImpl>().setChunkDirectory(false);
        if (without_records)
        {
            header.getRoot<zelph::network::ZelphImpl>().disownVariableRecord();
            header.getRoot<zelph::network::ZelphImpl>().disownPatternKinds();
        }
        const auto behind_header = input.tryGetReadBuffer();

        // The size of the directory stems from the segment table that begins
        // it.
        std::vector<std::uint64_t> table((std::min)(behind_header.size(), std::size_t{4096}) / sizeof(std::uint64_t));
        std::memcpy(table.data(), behind_header.begin(), table.size() * sizeof(std::uint64_t));
        const std::size_t directory = sizeof(::capnp::word)
                                    * ::capnp::expectedSizeInWordsFromPrefix(kj::arrayPtr(reinterpret_cast<const ::capnp::word*>(table.data()), table.size()));
        REQUIRE(directory <= behind_header.size());

        kj::VectorOutputStream written;
        ::capnp::writePackedMessage(written, header);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(written.getArray().begin()), static_cast<std::streamsize>(written.getArray().size()));
        out.write(reinterpret_cast<const char*>(behind_header.begin() + directory), static_cast<std::streamsize>(behind_header.size() - directory));
    }

    // The number of bytes the packed header of the network in `file` occupies:
    // the position where its chunk directory or its first chunk begins.
    inline std::size_t header_bytes(const std::filesystem::path& file)
    {
        std::vector<kj::byte> bytes;
        {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        kj::ArrayInputStream input(kj::arrayPtr(bytes.data(), bytes.size()));
        {
            ::capnp::PackedMessageReader header(input);
        }
        return bytes.size() - input.tryGetReadBuffer().size();
    }

    // Whether the header of the network in `file` carries the record of the
    // template-variable store, and how many entries it holds (-1: none).
    inline long variable_record_entries(const std::filesystem::path& file)
    {
        std::vector<kj::byte> bytes;
        {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        kj::ArrayInputStream         input(kj::arrayPtr(bytes.data(), bytes.size()));
        ::capnp::PackedMessageReader read(input);
        const auto                   header = read.getRoot<zelph::network::ZelphImpl>();
        if (!header.hasVariableRecord()) return -1;
        return static_cast<long>(header.getVariableRecord().getNodes().size() + header.getVariableRecord().getRuleText().size());
    }
}
