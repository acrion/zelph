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

#include "io/chunk_directory.hpp"

#include "network/serialization_layout.hpp"

#include <capnp/serialize.h>

#include <algorithm>
#include <limits>

namespace zelph::io
{
    namespace
    {
        // The maximum number of entries a chunk in the left or right section
        // holds within `length` bytes, which also represents the maximum
        // amount a load reserves for it. Each entry written by the engine is
        // an AdjPair consisting of two words, neither of which is zero: the
        // node, since no node id equals 0, and the pointer to its adjacency,
        // which is a list pointer even when the list is empty. The packed
        // encoding spends at least two bytes for a non-zero word, comprising
        // a tag byte and the non-zero bytes of the word, or all eight bytes
        // in a run of words written as they are. Consequently, an entry
        // requires at least four bytes, and a chunk holds no more than a
        // quarter of its bytes in entries. A reader accepts list elements
        // narrower than two words, which hold more entries, but the engine
        // writes none.
        constexpr std::uint64_t max_adjacency_entries(const std::uint64_t length)
        {
            constexpr std::uint64_t min_entry_bytes = 4;
            return length / min_entry_bytes;
        }

        // A directory can occupy up to this many times the size of the one a
        // save writes for the header's chunk counts: a later engine might
        // list sections that this one does not recognize, which are kept by
        // number.
        constexpr std::uint64_t directory_size_multiple = 4;

        std::uint64_t directory_words(const std::vector<std::pair<network::FileSection, std::uint64_t>>& sections);

        bool is_adjacency(const std::uint16_t section)
        {
            return section == static_cast<std::uint16_t>(network::FileSection::LEFT)
                || section == static_cast<std::uint16_t>(network::FileSection::RIGHT);
        }
    }

    std::string section_name(const std::uint16_t section)
    {
        switch (static_cast<network::FileSection>(section))
        {
        case network::FileSection::LEFT:
            return "left";
        case network::FileSection::RIGHT:
            return "right";
        case network::FileSection::NAME_OF_NODE:
            return "nameOfNode";
        case network::FileSection::NODE_OF_NAME:
            return "nodeOfName";
        case network::FileSection::ANNOTATIONS:
            return "annotation";
        }
        return "section " + std::to_string(section);
    }

    const std::vector<DirectoryChunk>* ChunkDirectory::find(const network::FileSection section) const
    {
        for (const auto& listed : sections)
        {
            if (listed.section == static_cast<std::uint16_t>(section)) return &listed.chunks;
        }
        return nullptr;
    }

    std::vector<std::uint64_t> ChunkDirectory::entries(const network::FileSection section) const
    {
        std::vector<std::uint64_t> result;
        if (const auto* chunks = find(section))
        {
            result.reserve(chunks->size());
            for (const auto& chunk : *chunks)
                result.push_back(chunk.entries);
        }
        return result;
    }

    DeclaredChunks declared_chunks(const network::ZelphImpl::Reader& header)
    {
        return {header.getLeftChunkCount(),
                header.getRightChunkCount(),
                header.getNameOfNodeChunkCount(),
                header.getNodeOfNameChunkCount(),
                header.getAnnotationChunkCount()};
    }

    ChunkDirectory read_chunk_directory(kj::BufferedInputStream& input, const std::uint64_t header_bytes, const std::uint64_t readable_bytes, const DeclaredChunks& declared)
    {
        ChunkDirectory directory;
        directory.header_bytes = header_bytes;

        // Words as capnp reads them, held as 64-bit numbers, which have
        // their size and alignment.
        static_assert(sizeof(std::uint64_t) == sizeof(::capnp::word) && alignof(std::uint64_t) >= alignof(::capnp::word));
        const auto as_words = [](const std::vector<std::uint64_t>& buffer)
        { return kj::arrayPtr(reinterpret_cast<const ::capnp::word*>(buffer.data()), buffer.size()); };

        // The message's size derives from its segment table, which comes
        // first. Before any data of that magnitude is read or allocated, it
        // is compared to what remains in the stream, and to a small multiple
        // of the directory a save writes for the header's chunk counts,
        // including its segment table: what remains of a large file would
        // let a damaged table claim gigabytes.
        const std::uint64_t        available = readable_bytes > header_bytes ? readable_bytes - header_bytes : 0;
        const std::uint64_t        allowed   = directory_size_multiple
                                             * (directory_words({{network::FileSection::LEFT, declared.left},
                                                                 {network::FileSection::RIGHT, declared.right},
                                                                 {network::FileSection::NAME_OF_NODE, declared.name_of_node},
                                                                 {network::FileSection::NODE_OF_NAME, declared.node_of_name},
                                                                 {network::FileSection::ANNOTATIONS, declared.annotations}})
                                                + 1);
        std::vector<std::uint64_t> words;
        std::size_t                expected = 1;
        while (expected > words.size())
        {
            if (expected > allowed)
            {
                throw DirectoryUnreadable("the chunk directory needs " + std::to_string(expected * sizeof(::capnp::word))
                                          + " bytes behind the header, but the header's chunk counts allow at most "
                                          + std::to_string(allowed * sizeof(::capnp::word)));
            }
            if (expected > available / sizeof(::capnp::word))
            {
                throw DirectoryUnreadable("the chunk directory needs " + std::to_string(expected * sizeof(::capnp::word))
                                          + " bytes behind the header, but only " + std::to_string(available) + " follow");
            }
            const std::size_t have = words.size();
            words.resize(expected);
            input.read(words.data() + have, (expected - have) * sizeof(::capnp::word));
            expected = ::capnp::expectedSizeInWordsFromPrefix(as_words(words));
        }
        directory.directory_bytes = words.size() * sizeof(::capnp::word);

        try
        {
            ::capnp::FlatArrayMessageReader reader(as_words(words));
            for (const auto section : reader.getRoot<network::ChunkDirectory>().getSections())
            {
                DirectorySection& copy = directory.sections.emplace_back();
                copy.section           = static_cast<std::uint16_t>(section.getSection());
                for (const auto chunk : section.getChunks())
                    copy.chunks.push_back({chunk.getOffset(), chunk.getLength(), chunk.getEntries()});
            }
        }
        catch (const kj::Exception& e)
        {
            throw DirectoryUnreadable("what follows the header is not a chunk directory: " + std::string(e.getDescription().cStr()));
        }
        return directory;
    }

    std::vector<std::string> directory_deviations(const ChunkDirectory& directory, const DeclaredChunks& declared, const std::uint64_t file_size)
    {
        std::vector<std::string> deviations;

        // Each section declared by the header appears once, with as many
        // chunks as the header declares.
        const std::pair<network::FileSection, std::uint32_t> sections[] = {
            {network::FileSection::LEFT, declared.left},
            {network::FileSection::RIGHT, declared.right},
            {network::FileSection::NAME_OF_NODE, declared.name_of_node},
            {network::FileSection::NODE_OF_NAME, declared.node_of_name},
            {network::FileSection::ANNOTATIONS, declared.annotations},
        };
        for (const auto& [section, count] : sections)
        {
            const auto id    = static_cast<std::uint16_t>(section);
            const auto times = std::count_if(directory.sections.begin(), directory.sections.end(), [id](const DirectorySection& listed)
                                             { return listed.section == id; });
            if (times > 1)
            {
                deviations.push_back("the directory lists the " + section_name(id) + " section " + std::to_string(times) + " times");
            }
            const auto* chunks = directory.find(section);
            const auto  listed = chunks == nullptr ? std::size_t{0} : chunks->size();
            if (listed != count)
            {
                deviations.push_back("the directory lists " + std::to_string(listed) + " " + section_name(id) + " chunks, but the header declares "
                                     + std::to_string(count));
            }
        }

        // The directory's end is succeeded by the chunks, which proceed to the
        // file's end.
        std::uint64_t end      = directory.chunks_begin();
        std::string   previous = "the chunk directory";
        for (const auto& section : directory.sections)
        {
            for (std::size_t index = 0; index < section.chunks.size(); ++index)
            {
                const DirectoryChunk& chunk = section.chunks[index];
                const std::string     name  = chunk_name(section.section, index);
                if (chunk.offset != end)
                {
                    deviations.push_back(name + " begins at byte " + std::to_string(chunk.offset) + ", but " + previous + " ends at byte "
                                         + std::to_string(end));
                }
                if (chunk.length > std::numeric_limits<std::uint64_t>::max() - chunk.offset)
                {
                    deviations.push_back(name + " ends beyond the largest file there can be");
                    end = std::numeric_limits<std::uint64_t>::max();
                }
                else
                {
                    end = chunk.offset + chunk.length;
                }
                previous = name;

                if (is_adjacency(section.section) && chunk.entries > network::serialization::chunk_entries)
                {
                    deviations.push_back(name + " holds " + std::to_string(chunk.entries) + " entries, more than a chunk holds ("
                                         + std::to_string(network::serialization::chunk_entries) + ")");
                }
                if (is_adjacency(section.section) && chunk.entries > max_adjacency_entries(chunk.length))
                {
                    deviations.push_back(name + " holds " + std::to_string(chunk.entries) + " entries, more than its " + std::to_string(chunk.length)
                                         + " bytes can hold (" + std::to_string(max_adjacency_entries(chunk.length)) + ")");
                }
            }
        }
        if (end != file_size)
        {
            deviations.push_back(previous + " ends at byte " + std::to_string(end) + ", but the file ends at byte " + std::to_string(file_size));
        }
        return deviations;
    }

    std::string chunk_name(const std::uint16_t section, const std::uint64_t index)
    {
        return section_name(section) + " chunk " + std::to_string(index);
    }

    std::string describe_deviations(const std::vector<std::string>& deviations, const std::size_t limit)
    {
        std::string text;
        for (std::size_t i = 0; i < deviations.size() && i < limit; ++i)
        {
            if (i != 0) text += "; ";
            text += deviations[i];
        }
        if (deviations.size() > limit) text += "; and " + std::to_string(deviations.size() - limit) + " more";
        return text;
    }

    namespace
    {
        // The directory's contents consist of: the root pointer and the root
        // struct, one word each; the list of sections, a tag word along with
        // two words per section; and for each section, its list of chunks, a
        // tag word accompanied by three words per chunk. A message constructed
        // using this size forms a single segment.
        std::uint64_t directory_words(const std::vector<std::pair<network::FileSection, std::uint64_t>>& sections)
        {
            std::uint64_t words = 3 + 3 * sections.size();
            for (const auto& section : sections)
                words += 3 * section.second;
            return words;
        }
    }

    ChunkDirectoryWriter::ChunkDirectoryWriter(const std::vector<std::pair<network::FileSection, std::uint64_t>>& sections)
        : _message(static_cast<::capnp::uint>(directory_words(sections)))
    {
        auto listed = _message.initRoot<network::ChunkDirectory>().initSections(static_cast<::capnp::uint>(sections.size()));
        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            listed[static_cast<::capnp::uint>(i)].setSection(sections[i].first);
            listed[static_cast<::capnp::uint>(i)].initChunks(static_cast<::capnp::uint>(sections[i].second));
        }
    }

    void ChunkDirectoryWriter::record(const std::size_t position, const std::uint64_t index, const DirectoryChunk& chunk)
    {
        auto sections = _message.getRoot<network::ChunkDirectory>().getSections();
        if (position >= sections.size() || index >= sections[static_cast<::capnp::uint>(position)].getChunks().size())
        {
            throw std::logic_error("Saving: chunk " + std::to_string(index) + " of section " + std::to_string(position)
                                   + " lies outside the chunk directory");
        }
        auto recorded = sections[static_cast<::capnp::uint>(position)].getChunks()[static_cast<::capnp::uint>(index)];
        recorded.setOffset(chunk.offset);
        recorded.setLength(chunk.length);
        recorded.setEntries(chunk.entries);
    }

    void ChunkDirectoryWriter::write(kj::OutputStream& output)
    {
        ::capnp::writeMessage(output, _message);
    }
}
