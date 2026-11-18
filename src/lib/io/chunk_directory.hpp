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

// The chunk directory within a .bin: the message located behind the header that
// specifies the position of each chunk and the number of entries it holds
// (ChunkDirectory in zelph.capnp). A save writes it, loads believe it once it
// matches the file and size the node map from it, and .stat-file and
// .index-file check it. All of them read and verify it via this file, so that
// each holds a directory to the same conditions and names what contradicts it
// in the same words.

#include "io/zelph.capnp.h"

#include <capnp/message.h>
#include <kj/io.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zelph::io
{
    // A buffered stream that tracks the number of bytes its readers have
    // consumed, marking where a message ends in the file: a reader that
    // reads ahead fills the buffer, and only the portion it removes is
    // counted.
    class CountingBufferedInputStream final : public kj::BufferedInputStream
    {
    public:
        explicit CountingBufferedInputStream(kj::InputStream& inner)
            : _buffered(inner)
        {
        }

        kj::ArrayPtr<const kj::byte> tryGetReadBuffer() override { return _buffered.tryGetReadBuffer(); }

        size_t tryRead(void* buffer, size_t minBytes, size_t maxBytes) override
        {
            const size_t n = _buffered.tryRead(buffer, minBytes, maxBytes);
            _count += n;
            return n;
        }

        void skip(size_t bytes) override
        {
            _buffered.skip(bytes);
            _count += bytes;
        }

        std::uint64_t bytes_read() const { return _count; }

    private:
        kj::BufferedInputStreamWrapper _buffered;
        std::uint64_t                  _count{0};
    };

    // An output stream that counts the bytes it forwards, which is where
    // each message of a save begins.
    class CountingOutputStream final : public kj::OutputStream
    {
    public:
        explicit CountingOutputStream(kj::OutputStream& inner)
            : _inner(inner)
        {
        }

        void write(const void* buffer, size_t size) override
        {
            _inner.write(buffer, size);
            _count += size;
        }

        void write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override
        {
            _inner.write(pieces);
            for (const auto piece : pieces)
                _count += piece.size();
        }

        std::uint64_t bytes_written() const { return _count; }

    private:
        kj::OutputStream& _inner;
        std::uint64_t     _count{0};
    };

    struct DirectoryChunk
    {
        std::uint64_t offset  = 0;
        std::uint64_t length  = 0;
        std::uint64_t entries = 0;
    };

    struct DirectorySection
    {
        // A FileSection, or a number that this engine cannot identify.
        std::uint16_t               section = 0;
        std::vector<DirectoryChunk> chunks;
    };

    // A chunk directory as a file holds it, with the location where it lies.
    struct ChunkDirectory
    {
        std::uint64_t                 header_bytes    = 0; // the header message positioned ahead of it
        std::uint64_t                 directory_bytes = 0; // the message pertaining to the directory itself
        std::vector<DirectorySection> sections;

        // The starting point for the first chunk.
        std::uint64_t chunks_begin() const { return header_bytes + directory_bytes; }

        // The section's chunks, or null if the directory omits it.
        const std::vector<DirectoryChunk>* find(network::FileSection section) const;

        // The entries contained within each chunk of a section, none if it is
        // not listed.
        std::vector<std::uint64_t> entries(network::FileSection section) const;
    };

    // The chunk counts a header declares, which its directory must
    // enumerate.
    struct DeclaredChunks
    {
        std::uint32_t left         = 0;
        std::uint32_t right        = 0;
        std::uint32_t name_of_node = 0;
        std::uint32_t node_of_name = 0;
        std::uint32_t annotations  = 0;
    };
    DeclaredChunks declared_chunks(const network::ZelphImpl::Reader& header);

    // A directory that is entirely unreadable, along with the reason.
    class DirectoryUnreadable : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // A load's refusal of a .bin due to its chunk directory: one that does
    // not match the file, cannot be read, or cannot be checked because the
    // file's size is unknown. It occurs before the load discards the
    // session's graph.
    class DirectoryRefusal : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // Reads the directory located immediately after a `header_bytes`-byte
    // header from `input`, which stands right after that header and holds
    // `readable_bytes` bytes from the file's beginning; `declared` is what
    // the header declares. Raises DirectoryUnreadable if the stream
    // terminates within the directory, if the directory claims more bytes
    // than the header's chunk counts allow, or if what stands there is not
    // one.
    ChunkDirectory read_chunk_directory(kj::BufferedInputStream& input, std::uint64_t header_bytes, std::uint64_t readable_bytes, const DeclaredChunks& declared);

    // What conflicts with a directory in a file of `file_size` bytes whose
    // header declares `declared`. The directory is reliable when nothing
    // does: its chunks follow the directory consecutively, with no gap or
    // overlap, and terminate precisely at the file's end, each section the
    // header declares contains exactly as many chunks, and no chunk of the
    // left or right section holds more entries than a chunk can hold or
    // than its bytes allow.
    std::vector<std::string> directory_deviations(const ChunkDirectory& directory, const DeclaredChunks& declared, std::uint64_t file_size);

    // How a message names a section, which occurs through the selector's name
    // in .load-partial for the four sections that possess one, and through the
    // term "annotation" for the annotations, which lack a selector; and how it
    // names chunk `index` of a section: "left chunk 0".
    std::string section_name(std::uint16_t section);
    std::string chunk_name(std::uint16_t section, std::uint64_t index);

    // The deviations in a single line for an error message, the first `limit`
    // of these, and the count of additional ones beyond that.
    std::string describe_deviations(const std::vector<std::string>& deviations, std::size_t limit = 8);

    // Writes the directory of a save: first with zeros, positioned ahead of
    // the chunks, and again over those zeros once all chunks have been
    // recorded. Unpacked, its message maintains identical size regardless of
    // what it records.
    class ChunkDirectoryWriter
    {
    public:
        // The sections in the sequence they appear in the file, each
        // accompanied by its respective chunk count.
        explicit ChunkDirectoryWriter(const std::vector<std::pair<network::FileSection, std::uint64_t>>& sections);

        // Records chunk `index` of the section at `position` in that order.
        void record(std::size_t position, std::uint64_t index, const DirectoryChunk& chunk);

        void write(kj::OutputStream& output);

    private:
        ::capnp::MallocMessageBuilder _message;
    };
}
