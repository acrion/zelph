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

#include "io/bin_inspect.hpp"

#include "zelph.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zelph::io
{
    namespace
    {
        class CountingInputStream : public kj::InputStream
        {
        public:
            explicit CountingInputStream(kj::InputStream& inner)
                : _inner(inner)
            {
            }

            size_t tryRead(void* buffer, size_t minBytes, size_t maxBytes) override
            {
                auto n = _inner.tryRead(buffer, minBytes, maxBytes);
                _count += n;
                return n;
            }

            void skip(size_t bytes) override
            {
                _inner.skip(bytes);
                _count += bytes;
            }

            uint64_t bytes_read() const
            {
                return _count;
            }

        private:
            kj::InputStream& _inner;
            uint64_t         _count{0};
        };

        class CountingBufferedInputStream : public kj::BufferedInputStream
        {
        public:
            explicit CountingBufferedInputStream(kj::InputStream& inner)
                : _buffered(inner)
            {
            }

            kj::ArrayPtr<const kj::byte> tryGetReadBuffer() override
            {
                return _buffered.tryGetReadBuffer();
            }

            size_t tryRead(void* buffer, size_t minBytes, size_t maxBytes) override
            {
                auto n = _buffered.tryRead(buffer, minBytes, maxBytes);
                _count += n;
                return n;
            }

            void skip(size_t bytes) override
            {
                _buffered.skip(bytes);
                _count += bytes;
            }

            uint64_t bytes_read() const
            {
                return _count;
            }

        private:
            kj::BufferedInputStreamWrapper _buffered;
            uint64_t                       _count{0};
        };
    }

    // Cap'n Proto and kj report through kj::Exception, whose what() carries a
    // C++ source location and a hex stack trace. Neither means anything to
    // someone who typed a file name, and the two failures they cover here --
    // the file is not a zelph .bin at all, or it stops in the middle -- are
    // exactly what such a person needs told.
    [[noreturn]] void rethrow_bin_error(const std::string&   command,
                                        const std::string&   filename,
                                        const kj::Exception& e)
    {
        const std::string detail(e.getDescription().cStr());
        throw std::runtime_error(
            "Command " + command + ": '" + filename
            + "' is not a readable zelph .bin file (truncated, or not a .bin at all): "
            + detail);
    }

    ::capnp::ReaderOptions make_bin_reader_options()
    {
        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;
        return options;
    }

    BinHeaderStats read_bin_header_stats(const std::string& command, const std::string& filename)
    {
        FILE* file = fopen(filename.c_str(), "rb");
        if (!file)
        {
            throw std::runtime_error("Command " + command + ": Failed to open file '" + filename + "'");
        }

        try
        {
            BinHeaderStats stats;
            stats.file_size_bytes = std::filesystem::file_size(filename);

            kj::FdInputStream           raw_input(fileno(file));
            CountingBufferedInputStream counting_input(raw_input);

            auto options = make_bin_reader_options();

            {
                ::capnp::PackedMessageReader main_message(counting_input, options);
                auto                         impl = main_message.getRoot<zelph::network::ZelphImpl>();

                stats.left_chunk_count   = impl.getLeftChunkCount();
                stats.right_chunk_count  = impl.getRightChunkCount();
                stats.name_of_node_count = impl.getNameOfNodeChunkCount();
                stats.node_of_name_count = impl.getNodeOfNameChunkCount();
            }
            // Measured after the reader is gone; see read_bin_index_data.
            stats.header_length_bytes = counting_input.bytes_read();

            // The counts are whatever the header says. They are believed
            // everywhere downstream, so rule out the ones the file cannot
            // possibly back -- a corrupted header that still parses declares
            // chunk counts in the billions.
            const uint64_t declared = static_cast<uint64_t>(stats.left_chunk_count)
                                    + stats.right_chunk_count
                                    + stats.name_of_node_count
                                    + stats.node_of_name_count;
            if (stats.header_length_bytes + declared * kMinimumChunkBytes > stats.file_size_bytes)
            {
                // No fclose here: the catch(...) below owns the handle.
                throw std::runtime_error(
                    "Command .stat-file: '" + filename + "' declares " + std::to_string(declared)
                    + " chunks, which do not fit in " + std::to_string(stats.file_size_bytes)
                    + " bytes -- the header is corrupted or the file is truncated");
            }

            fclose(file);
            return stats;
        }
        catch (const kj::Exception& e)
        {
            fclose(file);
            rethrow_bin_error(command, filename, e);
        }
        catch (...)
        {
            fclose(file);
            throw;
        }
    }

    BinIndexData read_bin_index_data(const std::string& command, const std::string& filename)
    {
        FILE* file = fopen(filename.c_str(), "rb");
        if (!file)
        {
            throw std::runtime_error("Command " + command + ": Failed to open file '" + filename + "'");
        }

        try
        {
            BinIndexData data;
            data.filename              = filename;
            data.stats.file_size_bytes = std::filesystem::file_size(filename);

            kj::FdInputStream           raw_input(fileno(file));
            CountingBufferedInputStream counting_input(raw_input);
            auto                        options = make_bin_reader_options();

            // A PackedMessageReader reads its segments lazily. Here we only touch the
            // root struct (chunkIndex / which / lang), which lives in segment 0, so
            // any further segments stay unread until the reader is destroyed. The
            // destructor then skips the remaining segments to position the stream at
            // the start of the next message. We must therefore measure the consumed
            // byte count *after* the reader has been destroyed; measuring it while
            // the reader is still alive yields a length that only covers segment 0,
            // which produces a gap before the next chunk for large multi-segment
            // chunks (those exceeding the 512 MiB first segment of saveToFile's
            // MallocMessageBuilder(1u << 26)).
            uint64_t header_offset = counting_input.bytes_read();
            {
                ::capnp::PackedMessageReader main_message(counting_input, options);
                auto                         impl = main_message.getRoot<zelph::network::ZelphImpl>();
                data.stats.left_chunk_count       = impl.getLeftChunkCount();
                data.stats.right_chunk_count      = impl.getRightChunkCount();
                data.stats.name_of_node_count     = impl.getNameOfNodeChunkCount();
                data.stats.node_of_name_count     = impl.getNodeOfNameChunkCount();
            }
            data.header_length_bytes = counting_input.bytes_read() - header_offset;

            auto read_adj_chunks = [&](uint32_t                  count,
                                       std::vector<BinChunkRef>& target)
            {
                target.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint64_t    before = counting_input.bytes_read();
                    BinChunkRef ref;
                    ref.offset = before;
                    {
                        ::capnp::PackedMessageReader chunk_message(counting_input, options);
                        auto                         chunk = chunk_message.getRoot<zelph::network::AdjChunk>();
                        ref.chunk_index                    = chunk.getChunkIndex();
                        ref.which                          = chunk.getWhich().cStr();
                    }
                    // Measured after chunk_message is destroyed: the destructor has now
                    // skipped all remaining segments of this message.
                    ref.length = counting_input.bytes_read() - before;
                    target.push_back(std::move(ref));
                }
            };

            read_adj_chunks(data.stats.left_chunk_count, data.left_chunks);
            read_adj_chunks(data.stats.right_chunk_count, data.right_chunks);

            data.name_of_node_chunks.reserve(data.stats.name_of_node_count);
            for (uint32_t i = 0; i < data.stats.name_of_node_count; ++i)
            {
                uint64_t    before = counting_input.bytes_read();
                BinChunkRef ref;
                ref.offset = before;
                {
                    ::capnp::PackedMessageReader chunk_message(counting_input, options);
                    auto                         chunk = chunk_message.getRoot<zelph::network::NameChunk>();
                    ref.chunk_index                    = chunk.getChunkIndex();
                    ref.lang                           = chunk.getLang().cStr();
                }
                ref.length = counting_input.bytes_read() - before;
                data.name_of_node_chunks.push_back(std::move(ref));
            }

            data.node_of_name_chunks.reserve(data.stats.node_of_name_count);
            for (uint32_t i = 0; i < data.stats.node_of_name_count; ++i)
            {
                uint64_t    before = counting_input.bytes_read();
                BinChunkRef ref;
                ref.offset = before;
                {
                    ::capnp::PackedMessageReader chunk_message(counting_input, options);
                    auto                         chunk = chunk_message.getRoot<zelph::network::NodeNameChunk>();
                    ref.chunk_index                    = chunk.getChunkIndex();
                    ref.lang                           = chunk.getLang().cStr();
                }
                ref.length = counting_input.bytes_read() - before;
                data.node_of_name_chunks.push_back(std::move(ref));
            }

            fclose(file);
            return data;
        }
        catch (const kj::Exception& e)
        {
            fclose(file);
            rethrow_bin_error(command, filename, e);
        }
        catch (...)
        {
            fclose(file);
            throw;
        }
    }

    void write_bin_index_json(const BinIndexData& data, const std::string& output_filename)
    {
        std::ofstream out(output_filename);
        if (!out.is_open())
        {
            throw std::runtime_error("Command .index-file: Failed to open output file '" + output_filename + "'");
        }

        auto escape_json_string = [](const std::string& value)
        {
            std::ostringstream escaped;
            for (unsigned char ch : value)
            {
                switch (ch)
                {
                case '\"':
                    escaped << "\\\"";
                    break;
                case '\\':
                    escaped << "\\\\";
                    break;
                case '\b':
                    escaped << "\\b";
                    break;
                case '\f':
                    escaped << "\\f";
                    break;
                case '\n':
                    escaped << "\\n";
                    break;
                case '\r':
                    escaped << "\\r";
                    break;
                case '\t':
                    escaped << "\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        escaped << "\\u"
                                << std::hex
                                << std::setw(4)
                                << std::setfill('0')
                                << static_cast<int>(ch)
                                << std::dec
                                << std::setfill(' ');
                    }
                    else
                    {
                        escaped << static_cast<char>(ch);
                    }
                    break;
                }
            }
            return escaped.str();
        };

        auto write_chunk_array = [&](const char* key, const std::vector<BinChunkRef>& refs)
        {
            out << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < refs.size(); ++i)
            {
                const auto& ref = refs[i];
                out << "    {\"chunkIndex\":" << ref.chunk_index
                    << ",\"offset\":" << ref.offset
                    << ",\"length\":" << ref.length;
                if (!ref.which.empty())
                {
                    out << ",\"which\":\"" << escape_json_string(ref.which) << "\"";
                }
                if (!ref.lang.empty())
                {
                    out << ",\"lang\":\"" << escape_json_string(ref.lang) << "\"";
                }
                out << "}";
                if (i + 1 < refs.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ]";
        };

        out << "{\n";
        out << "  \"file\":\"" << escape_json_string(data.filename) << "\",\n";
        out << "  \"header\":{\"offset\":0,\"length\":" << data.header_length_bytes << "},\n";
        write_chunk_array("left", data.left_chunks);
        out << ",\n";
        write_chunk_array("right", data.right_chunks);
        out << ",\n";
        write_chunk_array("nameOfNode", data.name_of_node_chunks);
        out << ",\n";
        write_chunk_array("nodeOfName", data.node_of_name_chunks);
        out << "\n}\n";
    }
}
