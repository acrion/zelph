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

#include "read_async.hpp"

#include <bzlib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include <atomic>
#include <cstdlib>

#ifndef _WIN32
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace zelph::io;

struct Entry
{
    std::string    _line;
    std::streamoff _streampos;
};

// Internal batch configuration
static constexpr size_t BATCH_SIZE = 4096;

class ReadAsync::Impl
{
public:
    Impl(std::filesystem::path file_name, size_t sufficient_batch_count, DiagnosticCallback diagnostic_callback)
        : _sufficient_size(std::max<size_t>(2, sufficient_batch_count))
        , _file_name(std::move(file_name))
        , _diag(std::move(diagnostic_callback))
        , _compressed(_file_name.extension() == ".bz2")
    {
        std::ifstream file(_file_name, std::ifstream::ate | std::ifstream::binary);
        if (file.is_open())
        {
            _total_size = file.tellg();
            file.close();
            if (_diag)
            {
                _diag("[ReadAsync] File: " + _file_name.string()
                      + ", compressed: " + (_compressed ? "yes" : "no")
                      + ", queue capacity: " + std::to_string(_sufficient_size) + " batches of " + std::to_string(BATCH_SIZE));
            }
            _t = std::thread(&Impl::read_thread, this);
        }
        else
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _error_text = std::string("Could not open file '") + _file_name.string() + "' to get size";
            _eof        = true;
        }
    }

    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _stop_requested = true;
        }
        _cv_not_full.notify_all();
        if (_t.joinable()) _t.join();
    }

    void read_thread();

    // Pushes a full batch to the queue
    void put_batch(std::vector<Entry>&& batch);

    size_t                _sufficient_size;
    std::filesystem::path _file_name;
    bool                  _compressed;
    DiagnosticCallback    _diag;
    std::thread           _t;

    // Queue now stores batches (vectors) of entries
    std::queue<std::vector<Entry>> _queue;

    // Local cache for the consumer to iterate without locking
    std::vector<Entry> _consumer_cache;
    size_t             _consumer_index{0};

    bool                            _eof{false};
    bool                            _stop_requested{false};
    std::string                     _error_text;
    mutable std::mutex              _mtx;
    mutable std::condition_variable _cv_not_empty;
    mutable std::condition_variable _cv_not_full;
    std::streamsize                 _total_size{0};

    enum class DecompressorKind : int
    {
        Unknown = 0,
        Uncompressed,
        LibBzip2,
        Lbzip2,
        Pbzip2
    };

    std::atomic<uint64_t>         _stats_batches_enqueued{0};
    std::atomic<uint64_t>         _stats_entries_enqueued{0};
    mutable std::atomic<uint64_t> _stats_source_bytes_read{0};
    mutable std::atomic<uint64_t> _stats_output_bytes_emitted{0};
    std::atomic<uint64_t>         _stats_queue_wait_not_full_ns{0};
    std::atomic<uint64_t>         _stats_max_queue_size{0};
    std::atomic<int>              _stats_decompressor_kind{static_cast<int>(DecompressorKind::Unknown)};

private:
#ifndef _WIN32
    // Returns the name of a parallel bz2 decompressor if available, or empty string
    static std::string find_parallel_decompressor();

    // Parallel decompression via lbzip2/pbzip2. Returns true on success.
    bool read_compressed_parallel(const std::string&           decomp_cmd,
                                  std::vector<Entry>&          current_batch,
                                  const std::function<void()>& push_current_batch);
#endif

    // Single-threaded fallback using libbzip2
    void read_compressed_fallback(std::vector<Entry>&          current_batch,
                                  const std::function<void()>& push_current_batch) const;
};

std::streamsize ReadAsync::get_total_size() const
{
    return _pImpl->_total_size;
}

ReadAsync::ReadAsync(const std::filesystem::path& file_name, size_t sufficient_batch_count, DiagnosticCallback diagnostic_callback)
    : _pImpl(new Impl(file_name, sufficient_batch_count, std::move(diagnostic_callback)))
{
}

bool ReadAsync::get_batch(std::vector<std::pair<std::string, std::streamoff>>& lines) const
{
    std::unique_lock<std::mutex> lock(_pImpl->_mtx);
    _pImpl->_cv_not_empty.wait(lock, [&]
                               { return !_pImpl->_queue.empty() || _pImpl->_eof; });

    if (_pImpl->_queue.empty())
        return false;

    auto batch = std::move(_pImpl->_queue.front());
    _pImpl->_queue.pop();

    lock.unlock();
    _pImpl->_cv_not_full.notify_one();

    lines.clear();
    lines.reserve(batch.size());
    for (auto& e : batch)
    {
        lines.emplace_back(std::move(e._line), e._streampos);
    }
    return true;
}

ReadAsync::~ReadAsync()
{
    delete _pImpl;
}

std::string ReadAsync::error_text() const
{
    std::lock_guard<std::mutex> lck(_pImpl->_mtx);
    return _pImpl->_error_text;
}

// Helper to refill local consumer cache
bool refill_cache(ReadAsync::Impl* impl)
{
    std::unique_lock<std::mutex> lock(impl->_mtx);
    impl->_cv_not_empty.wait(lock, [&]
                             { return !impl->_queue.empty() || impl->_eof; });

    if (impl->_queue.empty())
        return false; // EOF and empty

    impl->_consumer_cache = std::move(impl->_queue.front());
    impl->_queue.pop();
    impl->_consumer_index = 0;

    lock.unlock();
    impl->_cv_not_full.notify_one();
    return true;
}

bool ReadAsync::get_line(std::string& line, std::streamoff& streampos) const
{
    // Check if we have data in the local cache
    if (_pImpl->_consumer_index >= _pImpl->_consumer_cache.size())
    {
        if (!refill_cache(_pImpl)) return false;
    }

    // No lock needed here, we are working on thread-local cache
    const auto& e = _pImpl->_consumer_cache[_pImpl->_consumer_index++];
    line          = e._line;
    streampos     = e._streampos;
    return true;
}

bool ReadAsync::get_line_utf8(std::string& line, std::streamoff& streampos) const
{
    if (_pImpl->_consumer_index >= _pImpl->_consumer_cache.size())
    {
        if (!refill_cache(_pImpl)) return false;
    }

    Entry& e  = _pImpl->_consumer_cache[_pImpl->_consumer_index++];
    line      = std::move(e._line);
    streampos = e._streampos;
    return true;
}

void ReadAsync::Impl::put_batch(std::vector<Entry>&& batch)
{
    std::unique_lock<std::mutex> lock(_mtx);

    const auto wait_begin = std::chrono::steady_clock::now();
    _cv_not_full.wait(lock, [&]
                      { return _queue.size() < _sufficient_size || _eof || _stop_requested; });
    _stats_queue_wait_not_full_ns.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - wait_begin)
                                  .count()),
        std::memory_order_relaxed);

    if (_stop_requested) return;

    const size_t batch_size = batch.size();
    _queue.push(std::move(batch));

    _stats_batches_enqueued.fetch_add(1, std::memory_order_relaxed);
    _stats_entries_enqueued.fetch_add(batch_size, std::memory_order_relaxed);

    uint64_t qsize   = static_cast<uint64_t>(_queue.size());
    uint64_t current = _stats_max_queue_size.load(std::memory_order_relaxed);
    while (qsize > current
           && !_stats_max_queue_size.compare_exchange_weak(current, qsize, std::memory_order_relaxed))
    {
    }

    lock.unlock();
    _cv_not_empty.notify_one();
}

#ifndef _WIN32
std::string ReadAsync::Impl::find_parallel_decompressor()
{
    // Prefer lbzip2 (generally faster), fall back to pbzip2
    for (const char* cmd : {"lbzip2", "pbzip2"})
    {
        std::string check = std::string("command -v ") + cmd + " >/dev/null 2>&1";
        if (std::system(check.c_str()) == 0)
            return cmd;
    }
    return {};
}

bool ReadAsync::Impl::read_compressed_parallel(
    const std::string&           decomp_cmd,
    std::vector<Entry>&          current_batch,
    const std::function<void()>& push_current_batch)
{
    // Create two pipes: parent feeds compressed data to child,
    // child writes decompressed data back to parent.
    // This lets us track the compressed file position accurately.
    int to_child[2], from_child[2];
    if (::pipe(to_child) != 0 || ::pipe(from_child) != 0)
        return false;

    pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        return false;
    }

    if (pid == 0)
    {
        // Child: wire pipes and exec decompressor in streaming mode
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::dup2(to_child[0], STDIN_FILENO);
        ::dup2(from_child[1], STDOUT_FILENO);
        ::close(to_child[0]);
        ::close(from_child[1]);
        ::execlp(decomp_cmd.c_str(), decomp_cmd.c_str(), "-dc", nullptr);
        ::_exit(127);
    }

    // Parent
    ::close(to_child[0]);
    ::close(from_child[1]);

    constexpr size_t buf_size = 256 * 1024;

    // Feeder thread: reads compressed file, writes to child stdin,
    // and exposes the current compressed-file position via atomic.
    std::atomic<std::streamoff> compressed_pos{0};

    std::thread feeder([&]()
                       {
        std::ifstream in(_file_name, std::ios::binary);
        std::vector<char> buf(buf_size);

        while (in && !_stop_requested)
        {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            auto n = in.gcount();
            if (n <= 0) break;
            
            _stats_source_bytes_read.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
               
            const char* p   = buf.data();
            ssize_t     rem = static_cast<ssize_t>(n);

            // cppcheck-suppress knownConditionTrueFalse
            while (rem > 0 && !_stop_requested)
            {
                ssize_t w = ::write(to_child[1], p, static_cast<size_t>(rem));
                if (w <= 0) goto feeder_done; // pipe broken (child exited)
                p   += w;
                rem -= w;
            }

            auto pos = in.tellg();
            if (pos >= 0)
                compressed_pos.store(pos, std::memory_order_relaxed);
        }

    feeder_done:
        ::close(to_child[1]); });

    // Main reader: parse decompressed output from child stdout into lines
    {
        std::vector<char> rbuf(buf_size);
        std::string       utf8_line;
        utf8_line.reserve(8192);
        int fd = from_child[0];

        while (!_stop_requested)
        {
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            ssize_t n = ::read(fd, rbuf.data(), rbuf.size());
            if (n <= 0) break;

            _stats_output_bytes_emitted.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

            const char* ptr  = rbuf.data();
            const char* end  = ptr + n;
            const char* last = ptr;

            std::streamoff pos = compressed_pos.load(std::memory_order_relaxed);

            while (ptr < end)
            {
                if (*ptr == '\n')
                {
                    utf8_line.append(last, static_cast<size_t>(ptr - last));

                    current_batch.push_back({std::move(utf8_line), pos});
                    if (current_batch.size() >= BATCH_SIZE)
                        push_current_batch();

                    utf8_line.clear();
                    utf8_line.reserve(8192);
                    last = ptr + 1;
                }
                ++ptr;
            }

            if (last < end)
                utf8_line.append(last, static_cast<size_t>(end - last));
        }

        if (!utf8_line.empty())
            current_batch.push_back({std::move(utf8_line), compressed_pos.load()});

        ::close(fd);
    }

    feeder.join();

    int status = 0;
    ::waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _error_text = std::string("Parallel decompressor (") + decomp_cmd
                    + ") exited with code " + std::to_string(WEXITSTATUS(status));
    }

    return true;
}
#endif

void ReadAsync::Impl::read_compressed_fallback(
    std::vector<Entry>&          current_batch,
    const std::function<void()>& push_current_batch) const
{
    constexpr size_t buf_size = 256 * 1024;

    std::ifstream stream(_file_name.string(), std::ios::binary);
    if (!stream) return;

    bz_stream strm = {nullptr};
    int       ret  = BZ2_bzDecompressInit(&strm, 0, 0);
    if (ret != BZ_OK) return;

    std::vector<char> inbuf(buf_size);
    std::vector<char> outbuf(buf_size);
    std::string       utf8_line;
    utf8_line.reserve(8192);

    strm.avail_in            = 0;
    strm.next_in             = nullptr;
    std::streamoff streampos = 0;

    while (!_stop_requested)
    {
        if (strm.avail_in == 0)
        {
            stream.read(inbuf.data(), buf_size);
            strm.avail_in = stream.gcount();
            strm.next_in  = inbuf.data();
            streampos     = stream.tellg();

            _stats_source_bytes_read.fetch_add(static_cast<uint64_t>(strm.avail_in), std::memory_order_relaxed);
            if (strm.avail_in == 0 && utf8_line.empty()) break;
        }

        strm.avail_out = buf_size;
        strm.next_out  = outbuf.data();

        ret = BZ2_bzDecompress(&strm);

        if (ret != BZ_OK && ret != BZ_STREAM_END) break;

        size_t have = buf_size - strm.avail_out;
        _stats_output_bytes_emitted.fetch_add(static_cast<uint64_t>(have), std::memory_order_relaxed);
        const char* ptr      = outbuf.data();
        const char* end      = ptr + have;
        const char* last_ptr = ptr;

        while (ptr < end)
        {
            if (*ptr == '\n')
            {
                utf8_line.append(last_ptr, ptr - last_ptr);

                current_batch.push_back({std::move(utf8_line), streampos});
                if (current_batch.size() >= BATCH_SIZE) push_current_batch();

                utf8_line.clear();
                utf8_line.reserve(8192);
                last_ptr = ptr + 1;
            }
            ++ptr;
        }
        if (last_ptr < end) utf8_line.append(last_ptr, end - last_ptr);

        if (ret == BZ_STREAM_END)
        {
            if (!utf8_line.empty())
            {
                current_batch.push_back({std::move(utf8_line), streampos});
            }
            break;
        }
    }
    BZ2_bzDecompressEnd(&strm);
}

void ReadAsync::Impl::read_thread()
{
    // Local batch buffer
    std::vector<Entry> current_batch;
    current_batch.reserve(BATCH_SIZE);

    auto push_current_batch = [&]()
    {
        if (!current_batch.empty())
        {
            put_batch(std::move(current_batch));
            current_batch = std::vector<Entry>();
            current_batch.reserve(BATCH_SIZE);
        }
    };

    if (_compressed)
    {
        bool used_parallel = false;
#ifndef _WIN32
        // Try parallel decompression first (lbzip2 or pbzip2)
        std::string parallel_cmd = find_parallel_decompressor();
        if (_diag)
        {
            _diag("[ReadAsync] Parallel decompressor: " + (parallel_cmd.empty() ? std::string("(none, using libbzip2 fallback)") : parallel_cmd));
        }

        if (!parallel_cmd.empty())
        {
            if (parallel_cmd == "lbzip2")
                _stats_decompressor_kind.store(static_cast<int>(DecompressorKind::Lbzip2), std::memory_order_relaxed);
            else if (parallel_cmd == "pbzip2")
                _stats_decompressor_kind.store(static_cast<int>(DecompressorKind::Pbzip2), std::memory_order_relaxed);

            used_parallel = read_compressed_parallel(parallel_cmd, current_batch, push_current_batch);
        }
#endif

        if (!used_parallel)
        {
            _stats_decompressor_kind.store(static_cast<int>(DecompressorKind::LibBzip2), std::memory_order_relaxed);

            if (_diag)
            {
                _diag("[ReadAsync] Using single-threaded libbzip2 fallback");
            }
            read_compressed_fallback(current_batch, push_current_batch);
        }
        else if (_diag)
        {
            _diag("[ReadAsync] Parallel decompression completed");
        }
    }
    else
    {
        _stats_decompressor_kind.store(static_cast<int>(DecompressorKind::Uncompressed), std::memory_order_relaxed);

        if (_diag)
        {
            _diag("[ReadAsync] Reading uncompressed file");
        }
        // Uncompressed path
        std::ifstream stream(_file_name, std::ios::binary);
        stream.rdbuf()->pubsetbuf(nullptr, 1024 * 1024);
        std::streamoff streampos = 0;
        std::string    line;

        // cppcheck-suppress accessMoved
        while (!_stop_requested && std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            current_batch.push_back({std::move(line), streampos});
            if (current_batch.size() >= BATCH_SIZE) push_current_batch();
            streampos = stream.tellg();
        }
    }

    // Push remaining items
    push_current_batch();

    {
        std::lock_guard<std::mutex> lck(_mtx);
        _eof = true;
    }
    _cv_not_empty.notify_all();
    _cv_not_full.notify_all();
}

ReadAsync::StatsSnapshot ReadAsync::get_stats_snapshot() const
{
    StatsSnapshot s;
    s.batches_enqueued       = _pImpl->_stats_batches_enqueued.load(std::memory_order_relaxed);
    s.entries_enqueued       = _pImpl->_stats_entries_enqueued.load(std::memory_order_relaxed);
    s.source_bytes_read      = _pImpl->_stats_source_bytes_read.load(std::memory_order_relaxed);
    s.output_bytes_emitted   = _pImpl->_stats_output_bytes_emitted.load(std::memory_order_relaxed);
    s.queue_wait_not_full_ns = _pImpl->_stats_queue_wait_not_full_ns.load(std::memory_order_relaxed);
    s.max_queue_size         = _pImpl->_stats_max_queue_size.load(std::memory_order_relaxed);
    s.queue_capacity         = _pImpl->_sufficient_size;
    s.compressed             = _pImpl->_compressed;

    switch (static_cast<ReadAsync::Impl::DecompressorKind>(_pImpl->_stats_decompressor_kind.load(std::memory_order_relaxed)))
    {
    case ReadAsync::Impl::DecompressorKind::Uncompressed:
        s.decompressor_name           = "none";
        s.using_external_decompressor = false;
        break;
    case ReadAsync::Impl::DecompressorKind::LibBzip2:
        s.decompressor_name           = "libbzip2";
        s.using_external_decompressor = false;
        break;
    case ReadAsync::Impl::DecompressorKind::Lbzip2:
        s.decompressor_name           = "lbzip2";
        s.using_external_decompressor = true;
        break;
    case ReadAsync::Impl::DecompressorKind::Pbzip2:
        s.decompressor_name           = "pbzip2";
        s.using_external_decompressor = true;
        break;
    default:
        s.decompressor_name           = "unknown";
        s.using_external_decompressor = false;
        break;
    }

    return s;
}
