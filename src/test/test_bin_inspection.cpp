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

#include <doctest/doctest.h>

#include "directory_rewrite.hpp"
#include "test_helpers.hpp"
#include "wikidata_dump.hpp"

#include "network/reasoning.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
    #include <fcntl.h>
    #include <pthread.h>
    #include <signal.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif
#ifdef __linux__
    #include <sys/resource.h>
#endif

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .stat-file and .index-file off the happy path.
//
// Both are the entry point of the sharding pipeline, so what they say about a
// file is believed by everything downstream. They used to answer a file that
// is not a .bin, or is empty, with the raw kj exception -- a C++ source
// location and a hex stack trace -- and .stat-file accepted any header,
// including one declaring more chunks than the file has bytes.
//
// The division of labour between the two is deliberate and now documented:
// .stat-file reads the header and the chunk directory that follows it, which
// enables it to respond instantly even on an 88 GB file. It holds the
// directory against the file's size and thus detects a file that has been cut
// short, but it cannot see a damaged chunk, nor can it identify issues in a
// file lacking a directory, as older engines produced them. .index-file reads
// every chunk and is the one that notices.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    struct Fixtures
    {
        fs::path root;
        fs::path good;      // a saved two-fact network
        fs::path empty;     // a saved network with nothing but the core nodes
        fs::path truncated; // good, cut after the header, within its chunk directory
        fs::path tiny;      // good, cut inside the header's own claims
        fs::path garbage;   // not a .bin at all
        fs::path nothing;   // zero bytes
    };

    void copy_prefix(const fs::path& from, const fs::path& to, std::streamsize bytes)
    {
        std::ifstream     in(from, std::ios::binary);
        std::vector<char> buffer(static_cast<size_t>(bytes));
        in.read(buffer.data(), bytes);
        std::ofstream(to, std::ios::binary).write(buffer.data(), in.gcount());
    }

    Fixtures build()
    {
        Fixtures f;
        f.root = fs::temp_directory_path() / "zelph_bin_inspection";
        fs::remove_all(f.root);
        fs::create_directories(f.root);

        f.good      = f.root / "good.bin";
        f.empty     = f.root / "empty.bin";
        f.truncated = f.root / "truncated.bin";
        f.tiny      = f.root / "tiny.bin";
        f.garbage   = f.root / "garbage.bin";
        f.nothing   = f.root / "nothing.bin";

        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
c p d
)");
            interactive.process(".save \"" + f.good.string() + "\"");
            interactive.process(".new");
            interactive.process(".save \"" + f.empty.string() + "\"");
        }

        copy_prefix(f.good, f.truncated, 200);
        copy_prefix(f.good, f.tiny, 40);
        std::ofstream(f.garbage, std::ios::binary) << "hello, this is not a serialized network";
        std::ofstream nothing(f.nothing, std::ios::binary); // created empty on purpose
        nothing.close();

        return f;
    }

    std::string read_text(const fs::path& path)
    {
        std::ifstream     in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // The number that `key` has in the first object located after `from` in the
    // JSON written by .index-file: number_after(index, "\"left\": [", "offset")
    // indicates the starting point of the first left chunk.
    std::uint64_t number_after(const std::string& index, const std::string& from, const std::string& key)
    {
        const std::size_t at = index.find("\"" + key + "\":", index.find(from));
        REQUIRE(at != std::string::npos);
        return std::stoull(index.substr(at + key.size() + 3));
    }

    // A copy of `from` with `extra` zero bytes appended after its end.
    void copy_longer(const fs::path& from, const fs::path& to, const std::size_t extra)
    {
        std::string bytes = read_text(from);
        bytes.append(extra, '\0');
        std::ofstream(to, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // Whether a command printed `line` exactly as a distinct line, character by
    // character.
    bool printed_line(const zelph::io::OutputCollector& collector, const std::string& line)
    {
        return std::any_of(collector.events().begin(), collector.events().end(), [&line](const zelph::io::OutputEvent& event)
                           { return event.channel == zelph::io::OutputChannel::Out && event.text == line; });
    }
}

namespace zelph::test
{
    // Binary files from older_networks/*.bin, compiled into this executable
    // through embed_files.cmake.
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

namespace
{
    // A network as an older engine stored it, omitting a chunk
    // directory, writing up to `bytes` bytes of it to `to`.
    void write_older_network(const fs::path& to, const std::size_t bytes)
    {
        const auto& networks = zelph::test::older_networks();
        const auto  network  = networks.find("data_collections");
        REQUIRE(network != networks.end());
        REQUIRE(bytes <= network->second.size());
        std::ofstream(to, std::ios::binary).write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(bytes));
    }
}

TEST_CASE("bin inspection: a file that is not a network is refused in words")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    for (const auto& bad : {f.garbage, f.nothing})
    {
        CHECK_THROWS_AS(interactive.process(".stat-file \"" + bad.string() + "\""), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".index-file \"" + bad.string() + "\" \""
                                            + (f.root / "out.json").string() + "\""),
                        std::runtime_error);

        // The message has to name the file and say what is wrong with it. The
        // kj detail may follow, the stack trace may not.
        try
        {
            interactive.process(".stat-file \"" + bad.string() + "\"");
            FAIL("expected .stat-file to refuse " << bad.string());
        }
        catch (const std::runtime_error& e)
        {
            const std::string message(e.what());
            CHECK(message.find(bad.filename().string()) != std::string::npos);
            CHECK(message.find("zelph .bin") != std::string::npos);
            CHECK(message.find("stack:") == std::string::npos);
        }
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a header claiming more than the file holds is refused")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // 40 bytes hold the header, whose four chunk counts cannot possibly be
    // backed by what is left. That is decidable without reading a chunk.
    try
    {
        interactive.process(".stat-file \"" + f.tiny.string() + "\"");
        FAIL("expected .stat-file to refuse a header that does not fit");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("chunks") != std::string::npos);
        CHECK(message.find("truncated") != std::string::npos);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: the header report and the chunk walk divide the work")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Happy path: four chunks, and the report says where the numbers come from.
    collector.clear();
    interactive.process(".stat-file \"" + f.good.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(any_output_contains(collector, "declared by the header"));

    // A network with only the core nodes has no names to write, so two of the
    // four sections are empty -- a shape the pipeline has to accept.
    collector.clear();
    interactive.process(".stat-file \"" + f.empty.string() + "\"");
    CHECK(any_output_contains(collector, "Name-of-Node Chunks: 0"));
    CHECK(any_output_contains(collector, "Node-of-Name Chunks: 0"));

    // Truncated after the header: .stat-file reports what the header says,
    // and the chunk directory behind it, which it reads as well, that the
    // file ends before the directory does. It reads no chunk. This is the
    // documented division of labour, not an oversight -- pinned so that a
    // future change to either command has to be deliberate.
    collector.clear();
    interactive.process(".stat-file \"" + f.truncated.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(printed_line(collector, "Chunk Directory: does not match the file"));

    // .index-file refuses that file too, at its directory, before
    // reading a chunk.
    const fs::path json = f.root / "out.json";
    CHECK_THROWS_AS(interactive.process(".index-file \"" + f.truncated.string() + "\" \"" + json.string() + "\""), std::runtime_error);

    // What the walk alone sees is a chunk that cannot be read within a file of
    // the right size. The file is damaged in the way a disk or a transfer can
    // damage one: the first eight bytes of its last chunk are overwritten, yet
    // the file retains its size. Its directory still matches, so .stat-file has
    // nothing to report, and .index-file, which reads each chunk, is the one
    // that identifies the issue: it fails and writes no index. Without this
    // case, a walk concluding a section at the first chunk it could not read
    // would produce an index for such a file, leaving that section empty.
    interactive.process(".index-file \"" + f.good.string() + "\" \"" + json.string() + "\"");
    const std::uint64_t last_chunk = number_after(read_text(json), "\"nodeOfName\": [", "offset");
    fs::remove(json);
    const fs::path damaged = f.root / "damaged.bin";
    {
        std::string bytes = read_text(f.good);
        REQUIRE(last_chunk + 8 <= bytes.size());
        bytes.replace(static_cast<std::size_t>(last_chunk), 8, 8, '\xff');
        std::ofstream(damaged, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    collector.clear();
    interactive.process(".stat-file \"" + damaged.string() + "\"");
    CHECK(any_output_contains(collector, "Chunk Directory: matches the file"));
    CHECK_THROWS_AS(interactive.process(".index-file \"" + damaged.string() + "\" \"" + json.string() + "\""), std::runtime_error);
    CHECK_FALSE(fs::exists(json));

    // A file lacking a chunk directory leaves .stat-file with nothing to hold
    // against its size: if truncated, it still reports what its header
    // declares, and it is the walk that fails, at the point where the file
    // ends.
    const fs::path      older      = f.root / "older-cut.bin";
    const std::uint64_t older_size = zelph::test::older_networks().at("data_collections").size();
    write_older_network(older, static_cast<std::size_t>(older_size - 8));
    collector.clear();
    interactive.process(".stat-file \"" + older.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(any_output_contains(collector, "Chunk Directory: none"));
    CHECK_THROWS_AS(interactive.process(".index-file \"" + older.string() + "\" \"" + json.string() + "\""), std::runtime_error);
    CHECK_FALSE(fs::exists(json));

    fs::remove_all(f.root);
}

// Every file the engine writes includes a chunk directory, and it matches:
// .stat-file finds it in accord with the file's size, and .index-file, which
// walks every chunk and reports any discrepancy, finds each one at the position
// specified by the directory, as long as it says and with the correct number of
// entries. A save of selected predicates counts what it keeps apart from the
// network, a network comprising solely core nodes features empty name sections,
// and annotations make a fifth section. The range the index refers to as the
// header terminates at the start of the first chunk: it covers the directory,
// which a load through a manifest reads from that range.
TEST_CASE("bin inspection: every file the engine saves has a chunk directory that matches it")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const fs::path              slice     = f.root / "slice.bin";
    const fs::path              annotated = f.root / "annotated.bin";
    process_lines(interactive, R"(
a p b
c q d
%(zelph/annotate (zelph/fact "a" "p" "b") "source" "a sentence")
)");
    interactive.process(".save-predicates \"" + slice.string() + "\" p");
    interactive.process(".save \"" + annotated.string() + "\"");

    const fs::path json = f.root / "index.json";
    for (const fs::path& file : {f.good, f.empty, slice, annotated})
    {
        CAPTURE(file.string());
        collector.clear();
        interactive.process(".stat-file \"" + file.string() + "\"");
        CHECK(printed_line(collector, "Chunk Directory: matches the file"));

        interactive.process(".index-file \"" + file.string() + "\" \"" + json.string() + "\"");
        const std::string index = read_text(json);
        CHECK(number_after(index, "\"header\"", "length") == number_after(index, "\"left\": [", "offset"));
    }

    fs::remove_all(f.root);
}

#ifndef _WIN32
namespace
{
    // A named pipe, lacking any size until it is read to completion. A
    // thread writes `bytes` into it for the single reader that opens it,
    // as `cat file > pipe` would.
    class PipeSource
    {
    public:
        PipeSource(fs::path path, std::string bytes)
            : _path(std::move(path))
        {
            fs::remove(_path);
            REQUIRE(::mkfifo(_path.c_str(), 0600) == 0);
            _writer = std::thread([this, bytes = std::move(bytes)]
                                  {
                // A reader that shuts down the pipe early must not end the
                // process: the signal remains attached to this thread and
                // goes with it.
                sigset_t broken_pipe;
                sigemptyset(&broken_pipe);
                sigaddset(&broken_pipe, SIGPIPE);
                pthread_sigmask(SIG_BLOCK, &broken_pipe, nullptr);

                const int fd = ::open(_path.c_str(), O_WRONLY);
                _opened      = true;
                if (fd < 0) return;
                for (std::size_t written = 0; written < bytes.size();)
                {
                    const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
                    if (n <= 0) break;
                    written += static_cast<std::size_t>(n);
                }
                ::close(fd); });
        }

        ~PipeSource()
        {
            // A reader that never arrives keeps the writer stalled while
            // attempting to open the pipe, and opening it without blocking
            // allows the writer to proceed.
            while (!_opened)
            {
                const int fd = ::open(_path.c_str(), O_RDONLY | O_NONBLOCK);
                if (fd >= 0) ::close(fd);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            _writer.join();
            fs::remove(_path);
        }

        PipeSource(const PipeSource&)            = delete;
        PipeSource& operator=(const PipeSource&) = delete;

    private:
        fs::path          _path;
        std::atomic<bool> _opened{false};
        std::thread       _writer;
    };

    // A named pipe that a thread reads to its end for the single writer
    // that opens it, counting the bytes, as `cat pipe > file` would.
    class PipeSink
    {
    public:
        explicit PipeSink(fs::path path)
            : _path(std::move(path))
        {
            fs::remove(_path);
            REQUIRE(::mkfifo(_path.c_str(), 0600) == 0);
            _reader = std::thread([this]
                                  {
                const int fd = ::open(_path.c_str(), O_RDONLY);
                _opened      = true;
                if (fd < 0) return;
                char buffer[4096];
                for (ssize_t n = ::read(fd, buffer, sizeof(buffer)); n > 0; n = ::read(fd, buffer, sizeof(buffer)))
                    _received += static_cast<std::size_t>(n);
                ::close(fd); });
        }

        ~PipeSink()
        {
            finish();
            fs::remove(_path);
        }

        const fs::path& path() const { return _path; }

        // The bytes transmitted by the writer, after the pipe has been closed.
        std::size_t received()
        {
            finish();
            return _received;
        }

        PipeSink(const PipeSink&)            = delete;
        PipeSink& operator=(const PipeSink&) = delete;

    private:
        void finish()
        {
            if (!_reader.joinable()) return;
            // A writer that never came leaves the reader awaiting the pipe's
            // opening, and opening it without blocking lets the reader proceed.
            while (!_opened)
            {
                const int fd = ::open(_path.c_str(), O_WRONLY | O_NONBLOCK);
                if (fd >= 0) ::close(fd);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            _reader.join();
        }

        fs::path          _path;
        std::atomic<bool> _opened{false};
        std::size_t       _received = 0;
        std::thread       _reader;
    };
}

// A save writes the chunk directory ahead of the chunks by seeking back over
// its zeros once they are written, so the target must support seeking. A
// pipe cannot, and a save into one is rejected prior to the first byte: the
// reader at the other end receives nothing, instead of a whole file whose
// directory stays zero and which every load refuses, and the message
// explains the reason.
TEST_CASE("save: a target that cannot seek back is refused before the first byte")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");

    PipeSink pipe(fs::temp_directory_path() / "zelph_save_into_pipe.bin");
    CHECK_THROWS_WITH_AS(interactive.process(".save \"" + pipe.path().string() + "\""), doctest::Contains("it cannot seek back"), std::runtime_error);
    CHECK(pipe.received() == 0);
}
#endif

// The chunk directory specifies the position of each chunk, and the chunks must
// terminate where the file terminates. .stat-file holds the directory against
// the file's size and so tells a file cut short from a whole one without
// needing to read a chunk, and it indicates both where the chunks end and where
// the file ends. Bytes located after the last chunk represent a difference as
// well. A discrepancy unrelated to size, such as a chunk said to hold more
// entries than its bytes can accommodate, falls under the same line, which
// therefore says whether the directory matches the file, rather than its size.
// A file originating from an older engine lacks a directory, and the report
// states this explicitly: a load of such a file reserves nothing. The lines
// above, which scripts read, remain unchanged.
TEST_CASE("bin inspection: .stat-file says whether the chunk directory matches the file")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const std::uint64_t         size = fs::file_size(f.good);

    // The whole report for a file as the engine saved it, line by line.
    // Scripts read the lines from File Size to Total Chunks, based on their
    // wording and in this sequence, so no intervening content is permitted,
    // and the line for the directory appears beneath them. A file with
    // annotations includes an additional line, Annotation Chunks, positioned
    // between the name sections and the total.
    collector.clear();
    interactive.process(".stat-file \"" + f.good.string() + "\"");
    {
        const std::vector<std::string> expected = {"Serialized File Statistics:",
                                                   "------------------------",
                                                   "File: " + f.good.string(),
                                                   "File Size: " + std::to_string(size) + " bytes",
                                                   "Left Chunks: 1",
                                                   "Right Chunks: 1",
                                                   "Name-of-Node Chunks: 1",
                                                   "Node-of-Name Chunks: 1",
                                                   "Total Chunks: 4",
                                                   "Chunk Directory: matches the file",
                                                   "------------------------",
                                                   "(declared by the header; use .index-file to verify the chunks)"};
        std::vector<std::string>       printed;
        for (const auto& event : collector.events())
        {
            if (event.channel == zelph::io::OutputChannel::Out) printed.push_back(event.text);
        }
        REQUIRE(printed.size() == expected.size());
        for (std::size_t line = 0; line < expected.size(); ++line)
        {
            CAPTURE(line);
            CHECK(printed[line] == expected[line]);
        }
    }

    const fs::path cut = f.root / "cut.bin";
    copy_prefix(f.good, cut, static_cast<std::streamsize>(size - 8));
    collector.clear();
    interactive.process(".stat-file \"" + cut.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(printed_line(collector, "Chunk Directory: does not match the file"));
    CHECK(any_output_contains(collector, "nodeOfName chunk 0 ends at byte " + std::to_string(size) + ", but the file ends at byte " + std::to_string(size - 8)));

    const fs::path longer = f.root / "longer.bin";
    copy_longer(f.good, longer, 8);
    collector.clear();
    interactive.process(".stat-file \"" + longer.string() + "\"");
    CHECK(any_output_contains(collector, "nodeOfName chunk 0 ends at byte " + std::to_string(size) + ", but the file ends at byte " + std::to_string(size + 8)));

    const fs::path dense = f.root / "dense.bin";
    fs::copy_file(f.good, dense);
    std::uint64_t claimed = 0;
    rewrite_directory(dense, [&claimed](zelph::network::ChunkDirectory::Builder directory)
                      {
                          auto left = directory.getSections()[0].getChunks()[0];
                          claimed   = left.getLength() / 4 + 1;
                          left.setEntries(claimed); });
    collector.clear();
    interactive.process(".stat-file \"" + dense.string() + "\"");
    CHECK(printed_line(collector, "Chunk Directory: does not match the file"));
    CHECK(any_output_contains(collector, "left chunk 0 holds " + std::to_string(claimed) + " entries, more than its"));

    const fs::path older = f.root / "older.bin";
    write_older_network(older, zelph::test::older_networks().at("data_collections").size());
    collector.clear();
    interactive.process(".stat-file \"" + older.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(any_output_contains(collector, "Chunk Directory: none"));

    fs::remove_all(f.root);
}

// .index-file walks each chunk and holds the chunk directory against what it
// finds. A directory that matches the file's size may still be incorrect
// regarding a chunk, which .stat-file cannot see and a load would believe: an
// entry count it would reserve for, or a boundary separating two chunks. The
// walk finds either discrepancy, names it, and writes no index for such a
// file. The directory is rewritten as a damaged file or another writer could
// hold it.
TEST_CASE("bin inspection: .index-file refuses a chunk directory that its chunks contradict")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const fs::path              json = f.root / "index.json";

    SUBCASE("an entry count")
    {
        std::uint64_t held = 0;
        rewrite_directory(f.good, [&held](zelph::network::ChunkDirectory::Builder directory)
                          {
                              auto left = directory.getSections()[0].getChunks()[0];
                              held      = left.getEntries();
                              left.setEntries(held + 1); });
        collector.clear();
        interactive.process(".stat-file \"" + f.good.string() + "\"");
        CHECK(printed_line(collector, "Chunk Directory: matches the file"));
        CHECK_THROWS_WITH_AS(interactive.process(".index-file \"" + f.good.string() + "\" \"" + json.string() + "\""),
                             doctest::Contains("the directory gives left chunk 0 " + std::to_string(held + 1) + " entries, it holds " + std::to_string(held)),
                             std::runtime_error);
    }
    SUBCASE("a boundary between two chunks")
    {
        // Eight bytes from the left chunk are passed to the right chunk: the
        // chunks continue to follow one another and end where the file ends.
        std::uint64_t right_begins = 0;
        rewrite_directory(f.good, [&right_begins](zelph::network::ChunkDirectory::Builder directory)
                          {
                              auto left  = directory.getSections()[0].getChunks()[0];
                              auto right = directory.getSections()[1].getChunks()[0];
                              left.setLength(left.getLength() - 8);
                              right_begins = right.getOffset();
                              right.setOffset(right_begins - 8);
                              right.setLength(right.getLength() + 8); });
        collector.clear();
        interactive.process(".stat-file \"" + f.good.string() + "\"");
        CHECK(printed_line(collector, "Chunk Directory: matches the file"));
        CHECK_THROWS_WITH_AS(interactive.process(".index-file \"" + f.good.string() + "\" \"" + json.string() + "\""),
                             doctest::Contains("the directory places right chunk 0 at byte " + std::to_string(right_begins - 8) + ", the file has it at byte "
                                               + std::to_string(right_begins)),
                             std::runtime_error);
    }
    CHECK_FALSE(fs::exists(json));

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: the annotation section is counted")
{
    // Node annotations constitute a fifth section following the name
    // sections, appearing solely when the network includes any. Both
    // commands convey this information: the header's count, and the chunk
    // walk's place for the section, which runs from the last name chunk to
    // the end of the file. For every other file, they continue to express
    // the same details as before the section's existence, ensuring that
    // published networks and the indexes made from them remain unchanged.
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const fs::path annotated = f.root / "annotated.bin";
    process_lines(interactive, R"(
a p b
c p d
%(zelph/annotate (zelph/fact "a" "p" "b") "source" "a sentence")
)");
    interactive.process(".save \"" + annotated.string() + "\"");

    collector.clear();
    interactive.process(".stat-file \"" + annotated.string() + "\"");
    CHECK(any_output_contains(collector, "Annotation Chunks: 1"));
    CHECK(any_output_contains(collector, "Total Chunks: 5"));

    const fs::path json = f.root / "annotated.json";
    interactive.process(".index-file \"" + annotated.string() + "\" \"" + json.string() + "\"");
    std::string index;
    {
        std::ifstream     in(json);
        std::stringstream text;
        text << in.rdbuf();
        index = text.str();
    }
    const auto number_after = [&index](const std::size_t from, const std::string& key)
    {
        const std::size_t at = index.find("\"" + key + "\":", from);
        REQUIRE(at != std::string::npos);
        return std::stoull(index.substr(at + key.size() + 3));
    };
    const std::size_t section = index.find("\"annotations\": [");
    REQUIRE(section != std::string::npos);
    CHECK(number_after(section, "chunkIndex") == 0);
    CHECK(number_after(section, "offset") + number_after(section, "length") == fs::file_size(annotated));
    const std::size_t names = index.find("\"nodeOfName\": [");
    REQUIRE(names != std::string::npos);
    CHECK(number_after(names, "offset") + number_after(names, "length") == number_after(section, "offset"));

    SUBCASE("a file without annotations reports no section")
    {
        collector.clear();
        interactive.process(".stat-file \"" + f.good.string() + "\"");
        CHECK(any_output_contains(collector, "Total Chunks: 4"));
        CHECK_FALSE(any_output_contains(collector, "Annotation Chunks"));

        interactive.process(".index-file \"" + f.good.string() + "\" \"" + json.string() + "\"");
        std::ifstream     in(json);
        std::stringstream text;
        text << in.rdbuf();
        CHECK(text.str().find("annotations") == std::string::npos);
    }
    SUBCASE("a file cut inside the section is noticed by its directory")
    {
        // The header continues to assert the section, which .stat-file
        // believes; the chunk directory places the section's chunk, which
        // extends past the file's end, and the commands that read it say so.
        const fs::path cut = f.root / "annotated-cut.bin";
        copy_prefix(annotated, cut, static_cast<std::streamsize>(fs::file_size(annotated) - 8));
        collector.clear();
        interactive.process(".stat-file \"" + cut.string() + "\"");
        CHECK(any_output_contains(collector, "Total Chunks: 5"));
        CHECK(any_output_contains(collector, "annotation chunk 0 ends at byte"));
        CHECK_THROWS_AS(interactive.process(".index-file \"" + cut.string() + "\" \"" + json.string() + "\""), std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process(".load \"" + cut.string() + "\""), doctest::Contains("annotation chunk 0 ends at byte"), std::runtime_error);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a failed load says what happened to the network")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Upon reading the file's header, a load supersedes the graph, meaning
    // that a file interrupted mid-process leaves part of its content in the
    // graph and nothing of the session. Previously, the message was the kj
    // backtrace and conveyed no information regarding this; the recovery path
    // is .new, and the message now indicates this explicitly. A file the
    // engine writes is rejected before this stage if it is truncated (its
    // chunk directory does not match it, see below); a file from an older
    // engine lacks a directory and is read until it breaks off.
    const fs::path truncated = f.root / "older-truncated.bin";
    write_older_network(truncated, 200);
    try
    {
        interactive.process(".load \"" + truncated.string() + "\"");
        FAIL("expected .load to refuse a truncated file");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("zelph .bin") != std::string::npos);
        CHECK(message.find(".new") != std::string::npos);
        CHECK(message.find("stack:") == std::string::npos);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a file that is not a network leaves the session's graph as it was")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");

    // A file that is not a network encounters failure at the header stage,
    // before any interaction with the graph: the session keeps its graph, and
    // the message must refrain from redirecting the user to .new for a network
    // that remains present.
    try
    {
        interactive.process(".load \"" + f.garbage.string() + "\"");
        FAIL("expected .load to refuse a file that is not a network");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("zelph .bin") != std::string::npos);
        CHECK(message.find(".new") == std::string::npos);
    }

    collector.clear();
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    fs::remove_all(f.root);
}

#ifdef __linux__
namespace
{
    // Caps the address space of the process at the amount it occupies plus
    // `margin` bytes, for the duration the object remains alive: an allocation
    // beyond the margin then fails instead of being served.
    class AddressSpaceCap
    {
    public:
        explicit AddressSpaceCap(const std::uint64_t margin)
        {
            REQUIRE(getrlimit(RLIMIT_AS, &_before) == 0);
            std::uint64_t pages = 0;
            std::ifstream("/proc/self/statm") >> pages;
            REQUIRE(pages > 0);
            rlimit capped   = _before;
            capped.rlim_cur = static_cast<rlim_t>(pages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)) + margin);
            if (capped.rlim_cur > _before.rlim_max) capped.rlim_cur = _before.rlim_max;
            REQUIRE(setrlimit(RLIMIT_AS, &capped) == 0);
        }
        ~AddressSpaceCap() { setrlimit(RLIMIT_AS, &_before); }
        AddressSpaceCap(const AddressSpaceCap&)            = delete;
        AddressSpaceCap& operator=(const AddressSpaceCap&) = delete;

    private:
        rlimit _before{};
    };
}
#endif

// A load believes the chunk directory once it matches the file, and refuses a
// file whose directory does not match before the session's network is
// discarded, saying what contradicts it. A file cut short is the case that
// matters most -- a download that terminated prematurely, a disk that became
// full -- and the directory exposes it before any chunk is read: without it,
// the load discarded the session's network and then halted mid-file. Each
// subcase damages one property the check holds: where the chunks end, that the
// directory exists for reading, where a chunk begins, and how many chunks a
// section contains. The last two are rewritten as a damaged directory or
// another writer might hold them.
TEST_CASE("load: a file whose chunk directory does not match it is refused before it replaces the session's network")
{
    const Fixtures      f    = build();
    const std::uint64_t size = fs::file_size(f.good);
    const fs::path      file = f.root / "damaged.bin";
    fs::copy_file(f.good, file);

    const auto refused = [&file](const std::string& deviation)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("a p b");
        for (const std::string load : {".load", ".load-partial"})
        {
            CAPTURE(load);
            CHECK_THROWS_WITH_AS(interactive.process(load + " \"" + file.string() + "\""), doctest::Contains(deviation), std::runtime_error);
        }
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));
    };

    SUBCASE("a file cut short")
    {
        copy_prefix(f.good, file, static_cast<std::streamsize>(size - 8));
        refused("nodeOfName chunk 0 ends at byte " + std::to_string(size) + ", but the file ends at byte " + std::to_string(size - 8));
    }
    SUBCASE("a file with bytes behind its last chunk")
    {
        copy_longer(f.good, file, 8);
        refused("nodeOfName chunk 0 ends at byte " + std::to_string(size) + ", but the file ends at byte " + std::to_string(size + 8));
    }
    SUBCASE("a file cut inside its chunk directory")
    {
        copy_prefix(f.good, file, 200);
        refused("the chunk directory needs");
    }
    SUBCASE("a chunk that does not begin where the one before it ends")
    {
        std::uint64_t left_ends = 0;
        rewrite_directory(file, [&left_ends](zelph::network::ChunkDirectory::Builder directory)
                          {
                              auto right = directory.getSections()[1].getChunks()[0];
                              left_ends  = right.getOffset();
                              right.setOffset(left_ends + 1); });
        refused("right chunk 0 begins at byte " + std::to_string(left_ends + 1) + ", but left chunk 0 ends at byte " + std::to_string(left_ends));
    }
    SUBCASE("two chunks that overlap")
    {
        // The right chunk begins one byte prior to where the left chunk ends,
        // and is one byte longer for it: it terminates at the same point as
        // before, with no other variation. A verification requesting merely the
        // absence of a gap would permit it to pass.
        std::uint64_t left_ends = 0;
        rewrite_directory(file, [&left_ends](zelph::network::ChunkDirectory::Builder directory)
                          {
                              auto right = directory.getSections()[1].getChunks()[0];
                              left_ends  = right.getOffset();
                              right.setOffset(left_ends - 1);
                              right.setLength(right.getLength() + 1); });
        refused("right chunk 0 begins at byte " + std::to_string(left_ends - 1) + ", but left chunk 0 ends at byte " + std::to_string(left_ends));
    }
    SUBCASE("a section with fewer chunks than the header declares")
    {
        // The directory names its fourth section with a number that
        // no section has, thereby omitting any nodeOfName chunk
        // entirely.
        rewrite_directory(file, [](zelph::network::ChunkDirectory::Builder directory)
                          { directory.getSections()[3].setSection(static_cast<zelph::network::FileSection>(9)); });
        refused("the directory lists 0 nodeOfName chunks, but the header declares 1");
    }
    SUBCASE("a section with more chunks than the header declares")
    {
        // Names in two languages make two chunks within each name section.
        // The header is rewritten to declare a single nodeOfName chunk
        // where the directory lists two, as a damaged header might: a load
        // that believed the header would stop before the last chunk and
        // say nothing about the names it omitted.
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process("a p b");
            interactive.process(".name a de a_de");
            interactive.process(".save \"" + file.string() + "\"");
        }
        rewrite_header(file, [](zelph::network::ZelphImpl::Builder header)
                       {
                           REQUIRE(header.getNodeOfNameChunkCount() == 2);
                           header.setNodeOfNameChunkCount(1); });
        refused("the directory lists 2 nodeOfName chunks, but the header declares 1");
    }

    SUBCASE("a chunk directory larger than the header's chunk counts allow")
    {
        // One damaged field, the size of the directory's sole segment,
        // claims every byte behind the header of a network comprising a
        // hundred facts: many times the directory of the four chunks that
        // its header declares. In a large file, the identical field claims
        // gigabytes that the file holds, and the load refuses the claim
        // before reading or allocating any data of that size.
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            for (int i = 0; i < 100; ++i)
                interactive.process("s" + std::to_string(i) + " p o" + std::to_string(i));
            interactive.process(".save \"" + file.string() + "\"");
        }
        const std::uint64_t claimed = claim_for_directory(file);
        refused("the chunk directory needs " + std::to_string(claimed) + " bytes behind the header, but the header's chunk counts allow at most");
    }
#ifdef __linux__
    SUBCASE("a chunk directory that claims a gigabyte is refused without that much being allocated")
    {
        // The same damaged field, now claiming a gigabyte for the directory of
        // a file roughly a kilobyte in size. The load refuses the claim for its
        // size, and it must not allocate any data of that magnitude before
        // verifying it: that is the instant when a large file with such a field
        // would take gigabytes while the session's network remains in memory.
        // An allocation does not show in what a load reports, so the process's
        // address space is capped while the two loads run, a quarter of a
        // gigabyte beyond what the process occupies. A load that sized a buffer
        // based on the claim before checking it would not get the buffer, and
        // would end with that failure rather than the refusal. The session
        // exists before the cap, since making one requires its own address
        // space. Linux enforces this cap; where it does not, the case specifies
        // this, and the subcase above holds the refusal through its message
        // everywhere.
        constexpr std::uint32_t gigabyte_in_words = std::uint32_t{1} << 27;
        const std::uint64_t     claimed           = claim_for_directory(file, gigabyte_in_words);

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("a p b");
        {
            const AddressSpaceCap cap(std::uint64_t{1} << 28);
            // Volatile, or the compiler takes the pair consisting of malloc and
            // free away, and with it, the answer.
            void* volatile served = std::malloc(static_cast<std::size_t>(claimed));
            if (served != nullptr)
            {
                std::free(served);
                MESSAGE("the cap on the address space is not enforced here: whether the load allocates the claim goes unchecked");
            }
            for (const std::string load : {".load", ".load-partial"})
            {
                CAPTURE(load);
                CHECK_THROWS_WITH_AS(interactive.process(load + " \"" + file.string() + "\""),
                                     doctest::Contains("the chunk directory needs " + std::to_string(claimed)
                                                       + " bytes behind the header, but the header's chunk counts allow at most"),
                                     std::runtime_error);
            }
        }
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));
    }
#endif
    fs::remove_all(f.root);
}

#ifndef _WIN32
// A pipe possesses no size until it is read to completion. A file lacking a
// chunk directory needs none, and it loads from a pipe, entirely or
// partially. A file equipped with a directory is checked against its size, a
// detail a pipe cannot supply: the load refuses it after reading its header,
// prior to replacing the session's network, and explains the reason.
TEST_CASE("load: a file without a chunk directory loads from a pipe, and one with a directory is refused")
{
    const Fixtures f    = build();
    const fs::path pipe = f.root / "pipe.bin";

    SUBCASE("a file without a chunk directory loads")
    {
        const fs::path older = f.root / "older.bin";
        write_older_network(older, zelph::test::older_networks().at("data_collections").size());

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + older.string() + "\"");
        const std::size_t nodes = node_count(collector, interactive);
        REQUIRE(nodes > 0);

        for (const std::string load : {".load", ".load-partial"})
        {
            CAPTURE(load);
            interactive.process(".new");
            {
                PipeSource source(pipe, read_text(older));
                interactive.process(load + " \"" + pipe.string() + "\"");
            }
            CHECK(node_count(collector, interactive) == nodes);
        }
    }
    SUBCASE("a file with a chunk directory is refused, and the session's network stays")
    {
        // A manifest that names the pipe as its .bin is rejected in the same
        // manner.
        const fs::path manifest = f.root / "manifest.json";
        std::ofstream(manifest) << "{\"source\": {\"binPath\": \"" << pipe.generic_string() << "\"}, \"sections\": {\"left\": {\"chunks\": []}, "
                                << "\"right\": {\"chunks\": []}, \"nameOfNode\": {\"chunks\": []}, \"nodeOfName\": {\"chunks\": []}}}\n";

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("a p b");
        for (const std::string& load : {".load \"" + pipe.string() + "\"", ".load-partial \"" + pipe.string() + "\"", ".load-partial \"" + manifest.string() + "\""})
        {
            CAPTURE(load);
            PipeSource source(pipe, read_text(f.good));
            CHECK_THROWS_WITH_AS(interactive.process(load),
                                 doctest::Contains("the size of the file cannot be determined (it is not a regular file), so its chunk directory cannot be checked"),
                                 std::runtime_error);
        }
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));
    }

    fs::remove_all(f.root);
}
#endif

// A complete-file .load previously integrated the file into the session
// incrementally, node by node, due to the chunk loaders' clears being
// positioned beneath a macro with no definition. A fact within the session,
// whose ids the file did not reference, remained intact; a session name
// continued to point to whichever file node took its identifier (`bill` was
// reassigned to the file's `john`, and a typed `bill likes Y` queried john);
// and the file's variable counter assigned an identifier that a session
// variable still retained, causing the first query involving a variable to
// fail with "Network::var: Node 18446744073709551615 already in use". A
// cluster recorded before the load still retained its listed identifiers, and
// dropping it removed the loaded nodes that had reused those identifiers. A
// load now starts from a blank graph and discards any knowledge the session
// kept about the old one; the subcases take each piece in sequence, mirroring
// how a session reaches it.
TEST_CASE("load: a network file replaces the session's graph")
{
    const fs::path plain     = fs::temp_directory_path() / "zelph_load_replaces.bin";
    const fs::path rules     = fs::temp_directory_path() / "zelph_load_replaces_rules.bin";
    const fs::path self_fact = fs::temp_directory_path() / "zelph_load_replaces_self_fact.bin";
    const fs::path list      = fs::temp_directory_path() / "zelph_load_replaces_list.bin";
    const fs::path term      = fs::temp_directory_path() / "zelph_load_replaces_term.bin";
    const auto     save      = [](const fs::path& file, const std::string& lines)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, lines);
        interactive.process(".save \"" + file.string() + "\"");
    };
    save(plain, "john ~ man\njohn likes mary");
    // A rule, and a fact that remains unapplied: auto-run is off upon the
    // arrival of `john ~ man`, meaning the file holds no `john ~ mortal`.
    save(rules, "(X ~ man) => (X ~ mortal)\n.auto-run\njohn ~ man");
    // Three files in which the nodes take the ids of nodes registered by a
    // subcase for display, each built in the same order as the session's.
    save(self_fact, "x likes x");
    save(list, "x has <a b>");
    save(term, "(a andthen b) unless c");

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const auto                  session = [&interactive]
    {
        process_lines(interactive, R"(
bill ~ man
bill likes ann
p1 q1 r1
p2 q2 r2
p3 q3 r3
(X ~ man) => (X ~ mortal)
)");
    };

    SUBCASE("the session's facts, names and rules go")
    {
        session();
        interactive.process(".load \"" + plain.string() + "\"");

        collector.clear();
        interactive.process("X ~ man");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john ~ man"});
        CHECK_THROWS_WITH_AS(interactive.process(".node bill"), doctest::Contains("No node found with name 'bill'"), std::runtime_error);
        collector.clear();
        interactive.process("p1 q1 X");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found."));
    }
    SUBCASE("a variable query after the load answers, whatever variables the session made")
    {
        // The reported reproduction: a query leaves its variables within the
        // graph, and the first variable of a query following the load collided
        // with one of those.
        interactive.process("X p Y");
        interactive.process(".load \"" + plain.string() + "\"");
        collector.clear();
        CHECK_NOTHROW(interactive.process("X ~ man"));
        CHECK(answers_contain(collector, "john ~ man"));
    }
    SUBCASE("a variable a Janet block named before the load is a new one after it")
    {
        // Within a single Janet block, a symbol names a single variable.
        // Following the load, the file's rule assigns the variable's
        // identifier to its own `X`, and the block's next `'X` took that node
        // over. The load makes the symbol name a new variable.
        collector.clear();
        interactive.process(R"(%(do (def before (zelph/fact 'X "in" "nil")) (zelph/load ")" + rules.generic_string()
                            + R"(") (zelph/out (string/format "same=%j" (= before (zelph/fact 'X "in" "nil"))))))");
        CHECK(any_output_contains(collector, "same=false"));
    }
    SUBCASE("the node printed last is forgotten")
    {
        // The `.node` directive, when used without a parameter, displays the
        // most recently printed node. The entries `bill ~ woman` and the
        // file's `john ~ man` are built from nodes sharing identical
        // identifiers, hence the prior identifier refers to the fact stored in
        // the file, which has never been output.
        interactive.process("bill ~ woman");
        interactive.process(".load \"" + plain.string() + "\"");
        CHECK_THROWS_WITH_AS(interactive.process(".node"), doctest::Contains("no previous output node"), std::runtime_error);
    }
    SUBCASE("a predicate that only the file declares is one")
    {
        // Predicate detection reads a cached collection of relation types,
        // while the session constructs its own set based on its `p`. Preserved
        // throughout the load, this collection concealed each fact associated
        // with a predicate that only the file declares.
        interactive.process("a p b");
        interactive.process(".load \"" + plain.string() + "\"");
        collector.clear();
        interactive.process("X likes Y");
        CHECK(answers_contain(collector, "john likes mary"));
    }
    SUBCASE("the input focus is forgotten")
    {
        // The deduction filter reveals the logical consequences of the input.
        // Since `bill` and the file's `john` share an id, a focus maintained
        // throughout the load showed `john ~ mortal` as though it derived from
        // the session's input.
        process_lines(interactive, "(X ~ man) => (X ~ mortal)\nbill ~ woman");
        interactive.process(".load \"" + rules.string() + "\"");
        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(any_output_contains(collector, "(john ~ mortal) ⇐"));
        collector.clear();
        interactive.process("X ~ mortal");
        CHECK(answers_contain(collector, "john ~ mortal"));
    }
    SUBCASE("a cluster recorded before the load is forgotten")
    {
        // c1 contains the session's nodes, while c2 is active upon loading.
        // Both go: none of their identifiers names what they recorded any
        // more, and dropping c1 caused the loaded nodes to be dropped.
        process_lines(interactive, ".cluster c1\nbill ~ man\nbill likes ann\n.cluster c2\nann ~ woman");
        interactive.process(".load \"" + plain.string() + "\"");

        collector.clear();
        interactive.process(R"(%(zelph/out (string/format "active=%j clusters=%j" (zelph/cluster) (zelph/clusters))))");
        CHECK(any_output_contains(collector, "active=nil clusters=@[]"));
        CHECK_THROWS_WITH_AS(interactive.process(".cluster-drop c1"), doctest::Contains("unknown cluster 'c1'"), std::runtime_error);
        collector.clear();
        interactive.process("X likes Y");
        CHECK(answers_contain(collector, "john likes mary"));
    }
    SUBCASE("the Janet state and a registered keyword survive, and auto-run is off")
    {
        // What lies outside the graph remains after a load: Janet definitions,
        // a registered keyword, and the session's settings. Auto-run is
        // disabled, as a load has always done.
        session();
        interactive.process("%(def kept @{:k 1})");
        interactive.process(R"(%(zelph/register-keyword "vk" (fn [text] (zelph/out (string "VK got: " text)) nil)))");
        interactive.process(".lang en");
        interactive.process(".load \"" + plain.string() + "\"");
        CHECK_FALSE(interactive.is_auto_run_active());

        collector.clear();
        interactive.process(R"(%(zelph/out (string/format "kept=%j" kept)))");
        CHECK(any_output_contains(collector, "kept=@{:k 1}"));
        interactive.process("vk hello.");
        interactive.process("");
        CHECK(any_output_contains(collector, "VK got: hello."));
        interactive.process(".lang");
        CHECK(any_output_contains(collector, "The current language is 'en'"));
    }
    SUBCASE("a small file says that the load switched auto-run off and why, without calling itself large")
    {
        // A load switches auto-run off, regardless of the content it loads,
        // and it indicated this with the message "due to loading a large
        // dataset" even when the file contained just two facts. No mechanism
        // evaluates the size of a load, hence the message explains the
        // occurrence and the rationale applicable to every load.
        collector.clear();
        interactive.process(".load \"" + plain.string() + "\"");
        CHECK(any_output_contains(collector, "Auto-run has been disabled, as every .load disables it"));
        CHECK_FALSE(any_event_contains(collector, "large dataset"));
        CHECK_FALSE(interactive.is_auto_run_active());
    }
    SUBCASE("a self-fact whose predicate shares an id with a verbose one is sugar")
    {
        // zelph/no-selffact-sugar records a predicate via its identifier. The
        // session's `loves` and the file's `likes` are built in identical
        // sequence and share it, and the registration kept through the load
        // printed the file's self-fact entirely, as though `likes` functioned
        // as a term-forming operator.
        process_lines(interactive, "y loves y\n%(zelph/no-selffact-sugar \"loves\")");
        interactive.process(".load \"" + self_fact.string() + "\"");

        collector.clear();
        interactive.process("X likes Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{":likes x"});
    }
    SUBCASE("a list of nodes that share ids with the digits is a list")
    {
        // zelph/set-number-digits records the digits indexed by id, and a list
        // composed solely of these digits outputs as a numeric value. The
        // file's `a` and `b` take the ids corresponding to the session's
        // digits `c` and `d`, and the file's list printed as &2.
        process_lines(interactive, "y owns <c d>\n%(zelph/set-number-digits [\"c\" \"d\"])");
        interactive.process(".load \"" + list.string() + "\"");

        collector.clear();
        interactive.process("X has Y");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x has <a b>"});
    }
    SUBCASE("a display scheme goes with the operators it claimed")
    {
        // The scheme claims `r` and `s` by identifier, the file's `andthen`
        // and `unless` take those ids, and the file's term printed in the
        // scheme's notation. The claim kept through the load additionally
        // rejected the module's own registration of `andthen` upon subsequent
        // module execution ("already registered in scheme 'policy'"); the
        // scheme is likewise discarded, enabling the module to register it
        // anew.
        process_lines(interactive, "(p r q) s w\n"
                                   "%(do (zelph/register-display-scheme \"policy\" \"[[\" \"]]\" "
                                   "{:name-first \"abcdefghijklmnopqrstuvwxyz\" :name-chars \"abcdefghijklmnopqrstuvwxyz\"}) "
                                   "(zelph/set-infix-display \"policy\" [[\"r\" 20 :left] [\"s\" 10 :left]]))");
        interactive.process(".load \"" + term.string() + "\"");

        collector.clear();
        interactive.process("(A andthen B) unless C");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a andthen b) unless c"});
        CHECK_THROWS_WITH_AS(interactive.process(R"(%(zelph/set-infix-display "policy" [["andthen" 20 :left]]))"),
                             doctest::Contains("unknown display scheme 'policy'"),
                             std::runtime_error);
    }
    SUBCASE("the next run-delta is a full pass")
    {
        // The session has run, thus its delta record is valid, and the session
        // and the file contain an identical count of rules. Performing seeding
        // from that record after the load skipped the classic pass, and the
        // file's `john ~ man` never met the rule.
        process_lines(interactive, "(X ~ man) => (X ~ mortal)\nbill ~ man");
        interactive.process(".load \"" + rules.string() + "\"");

        collector.clear();
        interactive.process(".run-delta");
        CHECK(any_event_contains(collector, "Incremental run not applicable"));
        collector.clear();
        interactive.process("X ~ mortal");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john ~ mortal"});
    }
    SUBCASE("zelph/load replaces as .load does")
    {
        session();
        interactive.process("%(zelph/load \"" + plain.generic_string() + "\")");

        collector.clear();
        interactive.process("X ~ man");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john ~ man"});
        CHECK_THROWS_WITH_AS(interactive.process(".node bill"), doctest::Contains("No node found with name 'bill'"), std::runtime_error);
    }

    fs::remove(plain);
    fs::remove(rules);
    fs::remove(self_fact);
    fs::remove(list);
    fs::remove(term);
}

// Annotations associated with a session are tied to its nodes, and a load
// replaces them. Node identifiers are positions: the session's `x`
// and the file's `john` take the same identifier, meaning an annotation
// referencing x that persisted past the load would be interpreted as
// belonging to john. The file's original annotations occupy their position
// instead; a partial load retrieves none, since a view contains no
// annotations.
TEST_CASE("annotations: a replacing load takes them along")
{
    const fs::path annotated = fs::temp_directory_path() / "zelph_load_annotations.bin";
    const fs::path bare      = fs::temp_directory_path() / "zelph_load_annotations_bare.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
john ~ man
%(zelph/annotate "john" "source" "from the file")
)");
        interactive.process(".save \"" + annotated.string() + "\"");
    }
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("john ~ man");
        interactive.process(".save \"" + bare.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
x ~ man
%(def x (zelph/lookup "x"))
%(zelph/annotate x "source" "from the session")
)");

    SUBCASE("a file with annotations")
    {
        interactive.process(".load \"" + annotated.string() + "\"");
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "john")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == R"({"source" ("from the file")})");
    }
    SUBCASE("a file without annotations")
    {
        interactive.process(".load \"" + bare.string() + "\"");
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "john")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == "{}");
    }
    SUBCASE("a partial load")
    {
        interactive.process(".load-partial \"" + annotated.string() + "\"");
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "john")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == "{}");
    }
    SUBCASE("a file that is not a network")
    {
        // A file that does not constitute a network is rejected at the
        // header stage, before any interaction with the graph: the session
        // keeps its graph, along with the annotations attached to its nodes.
        const fs::path garbage = fs::temp_directory_path() / "zelph_load_annotations_garbage.bin";
        std::ofstream(garbage, std::ios::binary) << "hello, this is not a serialized network";
        CHECK_THROWS_AS(interactive.process(".load \"" + garbage.string() + "\""), std::runtime_error);
        REQUIRE(janet_value(collector, interactive, R"((= x (zelph/lookup "x")))") == "true");
        CHECK(janet_value(collector, interactive, "(zelph/annotations x)") == R"({"source" ("from the session")})");
        fs::remove(garbage);
    }

    fs::remove(annotated);
    fs::remove(bare);
}

TEST_CASE("bin inspection: a mistyped chunk selector leaves the session alone")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // The selectors are checked before the graph is touched. They used to be
    // checked after it had been discarded, which left a network without even
    // its core nodes: every following statement failed with "requested left
    // node 1 does not exist", and nothing said that .new was the way out.
    CHECK_THROWS_AS(interactive.process(".load-partial \"" + f.good.string() + "\" left=99"),
                    std::runtime_error);

    collector.clear();
    interactive.process("a p b");
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    fs::remove_all(f.root);
}

TEST_CASE("load-partial: a partial load marks the label index incomplete")
{
    // Every load retrieves the carriers associated with a shared label,
    // drawing from both name sections. When a load examines only a portion of
    // their chunks, it identifies only a subset of the carriers, and
    // responding based on that partial set would constitute a silent error;
    // the response indicates it cannot provide an answer.
    using namespace wikidata_dump;
    const auto     dump = write_lines("zelph_partial_labels",
                                      {entity("Q8441", text("en", "man")),
                                       entity("Q29719014", text("en", "man")),
                                       entity("Q709369", text("en", "man"))});
    const fs::path file = fs::temp_directory_path() / "zelph_partial_labels_saved.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    interactive.process(".save \"" + file.string() + "\"");
    const std::vector<std::string> carriers{"Q8441", "Q709369", "Q29719014"};

    SUBCASE("a selection of the name chunks")
    {
        interactive.process(".load-partial \"" + file.string() + "\" nameOfNode=0 nodeOfName=0");
        CHECK_FALSE(interactive.graph()->nodes_named("man", "en").has_value());

        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == carriers);
    }

    SUBCASE("a whole load after it")
    {
        // A whole load replaces the graph that the partial one left
        // behind, along with its associated mark: there is no need for a
        // .new in between.
        interactive.process(".load-partial \"" + file.string() + "\" nameOfNode=0 nodeOfName=0");
        CHECK_FALSE(interactive.graph()->nodes_named("man", "en").has_value());

        interactive.process(".load \"" + file.string() + "\"");
        CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == carriers);
    }

    SUBCASE("every name chunk")
    {
        interactive.process(".load-partial \"" + file.string() + "\" left=0");
        CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == carriers);
    }

    fs::remove(file);
    remove_dump(dump);
}

// A file holds the outgoing edges of its nodes (the left section) apart from
// their incoming edges (the right section), and a partial load can read either
// section independently, while the engine keeps both sides of a node within a
// single entry in one map (Network::node_entry). The definition of a node is
// determined by its left side: a node is considered present when that side has
// been read, which is precisely what `.stat` counts and what `.list` lists,
// and a side that was not read contains no edges to report. Consequently, a
// view comprising only the right sections contains edges but no node; counting
// the entries in the map would instead tally every node present in the file.
//
// A statement entered within a view of one side is rejected, because an edge
// needs both of its endpoints: if written into a side that was never read, it
// would be treated as the whole of that side once the view is saved.
TEST_CASE("load-partial: a node exists where its left side was read")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + f.good.string() + "\"");
    const std::size_t nodes = node_count(collector, interactive);
    REQUIRE(nodes > 0);

    const auto shows = [&](const std::string& command, const std::string& text)
    {
        collector.clear();
        interactive.process(command);
        return any_output_contains(collector, text);
    };

    SUBCASE("the left sections alone")
    {
        interactive.process(".load-partial \"" + f.good.string() + "\" right=none");
        CHECK(node_count(collector, interactive) == nodes);
        CHECK(shows(".list 1000", "Displayed " + std::to_string(nodes) + " nodes."));
        CHECK(shows(".out a", "(first 1 of 1"));
        CHECK(shows(".in a", "(first 0 of 0"));
        CHECK_THROWS_AS(interactive.process("q1 q2 q3"), std::runtime_error);
    }

    SUBCASE("the right sections alone")
    {
        interactive.process(".load-partial \"" + f.good.string() + "\" left=none");
        CHECK(node_count(collector, interactive) == 0);
        CHECK(shows(".list 1000", "Displayed 0 nodes."));
        CHECK(shows(".in a", "(first 1 of 1"));
        CHECK(shows(".out a", "(first 0 of 0"));
        CHECK_THROWS_AS(interactive.process("q1 q2 q3"), std::runtime_error);
    }

    fs::remove_all(f.root);
}

// The REPL rejects `.save` within a partial view, yet a program utilizing the
// library is not bound by this restriction: it loads a selection of chunks and
// saves the resulting state (Zelph::load_from_file, Zelph::save_to_file). A
// save writes each side of the entries that hold it, so the file of a
// one-sided view contains one empty section, and loading that file as a whole
// gives the identical view back: a node of which only the right side was read
// is still no node.
TEST_CASE("load-partial: a saved view writes the sides it read")
{
    const Fixtures f          = build();
    const fs::path left_only  = f.root / "left-only.bin";
    const fs::path right_only = f.root / "right-only.bin";

    std::size_t nodes = 0;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + f.good.string() + "\"");
        nodes = node_count(collector, interactive);
    }

    for (const auto& [file, keep_left] : {std::pair{left_only, true}, std::pair{right_only, false}})
    {
        zelph::network::Zelph                    z([](const zelph::io::OutputEvent&) {});
        zelph::network::Zelph::BinChunkSelection selection;
        selection.left_explicit  = !keep_left; // an explicit, empty selection is `=none`
        selection.right_explicit = keep_left;
        z.load_from_file(f.good.string(), selection);
        z.save_to_file(file.string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const auto                  shows = [&](const std::string& command, const std::string& text)
    {
        collector.clear();
        interactive.process(command);
        return any_output_contains(collector, text);
    };

    CHECK(shows(".stat-file \"" + left_only.string() + "\"", "Left Chunks: 1"));
    CHECK(shows(".stat-file \"" + left_only.string() + "\"", "Right Chunks: 0"));
    CHECK(shows(".stat-file \"" + right_only.string() + "\"", "Left Chunks: 0"));
    CHECK(shows(".stat-file \"" + right_only.string() + "\"", "Right Chunks: 1"));

    interactive.process(".load \"" + left_only.string() + "\"");
    CHECK(node_count(collector, interactive) == nodes);
    interactive.process(".load \"" + right_only.string() + "\"");
    CHECK(node_count(collector, interactive) == 0);
    CHECK(shows(".in a", "(first 1 of 1"));

    fs::remove_all(f.root);
}
