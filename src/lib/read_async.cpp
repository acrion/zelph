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

#include "string_utils.hpp"

#include <bzlib.h>

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <thread>

using namespace zelph::console;

struct Entry
{
    std::wstring   _line;
    std::streamoff _streampos;
};

class ReadAsync::Impl
{
public:
    Impl(std::filesystem::path file_name, size_t sufficient_size)
        : _sufficient_size(sufficient_size)
        , _file_name(std::move(file_name))
        , _compressed(_file_name.extension() == ".bz2")
    {
        std::ifstream file(_file_name, std::ifstream::ate | std::ifstream::binary);
        if (file.is_open())
        {
            _total_size = file.tellg();
            file.close();
            _t = std::thread(&Impl::read_thread, this);
        }
        else
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _error_text = std::string("Could not open file '") + _file_name.string() + "' to get size";
            _EOF        = true;
        }
    }

    ~Impl()
    {
        if (_t.joinable()) _t.join();
    }

    void read_thread();
    void put_line(Entry entry);

    size_t                  _sufficient_size;
    std::filesystem::path   _file_name;
    bool                    _compressed;
    std::thread             _t;
    std::queue<Entry>       _lines;
    bool                    _EOF{false};
    std::string             _error_text;
    std::mutex              _mtx;
    std::condition_variable _cv_not_empty;
    std::condition_variable _cv_not_full;
    std::streamsize         _total_size{0};
};

std::streamsize ReadAsync::get_total_size() const
{
    return _pImpl->_total_size;
}

ReadAsync::ReadAsync(const std::filesystem::path& file_name, size_t sufficient_size)
    : _pImpl(new Impl(file_name, sufficient_size))
{
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

bool ReadAsync::get_line(std::wstring& line, std::streamoff& streampos) const
{
    std::unique_lock<std::mutex> lock(_pImpl->_mtx);
    _pImpl->_cv_not_empty.wait(lock, [&]
                               { return !_pImpl->_lines.empty() || _pImpl->_EOF; });

    if (_pImpl->_lines.empty())
        return false;

    Entry e = std::move(_pImpl->_lines.front());
    _pImpl->_lines.pop();

    lock.unlock();
    _pImpl->_cv_not_full.notify_one();

    line      = std::move(e._line);
    streampos = e._streampos;
    return true;
}

void ReadAsync::Impl::put_line(Entry entry)
{
    std::unique_lock<std::mutex> lock(_mtx);
    _cv_not_full.wait(lock, [&]
                      { return _lines.size() < _sufficient_size || _EOF; });

    if (_EOF) return;

    _lines.push(std::move(entry));
    lock.unlock();
    _cv_not_empty.notify_one();
}

void ReadAsync::Impl::read_thread()
{
    if (_compressed)
    {
        std::ifstream stream(_file_name.string(), std::ios::binary);
        if (!stream)
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _error_text = "Could not open compressed file '" + _file_name.string() + "'";
            _EOF        = true;
            _cv_not_empty.notify_all();
            _cv_not_full.notify_all();
            return;
        }

        bz_stream strm = {nullptr};
        strm.bzalloc   = nullptr;
        strm.bzfree    = nullptr;
        strm.opaque    = nullptr;

        int ret = BZ2_bzDecompressInit(&strm, 0, 0);
        if (ret != BZ_OK)
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _error_text = "BZ2_bzDecompressInit failed";
            _EOF        = true;
            _cv_not_empty.notify_all();
            _cv_not_full.notify_all();
            return;
        }

        const size_t buf_size = 4096;
        char         inbuf[buf_size];
        char         outbuf[buf_size];
        std::string  utf8_line;

        strm.avail_in = 0;
        strm.next_in  = nullptr;

        std::streamoff streampos = 0;

        while (true)
        {
            if (strm.avail_in == 0)
            {
                stream.read(inbuf, buf_size);
                strm.avail_in = stream.gcount();
                strm.next_in  = inbuf;
                streampos     = stream.tellg();
                if (strm.avail_in == 0 && utf8_line.empty()) break;
            }

            strm.avail_out = buf_size;
            strm.next_out  = outbuf;

            ret = BZ2_bzDecompress(&strm);

            if (ret == BZ_MEM_ERROR || ret == BZ_PARAM_ERROR)
            {
                std::lock_guard<std::mutex> lck(_mtx);
                _error_text = "BZ2_bzDecompress error";
                _EOF        = true;
                BZ2_bzDecompressEnd(&strm);
                _cv_not_empty.notify_all();
                _cv_not_full.notify_all();
                return;
            }

            size_t have = buf_size - strm.avail_out;
            utf8_line.append(outbuf, have);

            size_t pos;
            while ((pos = utf8_line.find('\n')) != std::string::npos)
            {
                std::wstring line = string::unicode::from_utf8(utf8_line.substr(0, pos));
                put_line(Entry{std::move(line), streampos});
                utf8_line.erase(0, pos + 1);
            }

            if (ret == BZ_STREAM_END)
            {
                if (!utf8_line.empty())
                {
                    std::wstring line = string::unicode::from_utf8(utf8_line);
                    put_line(Entry{std::move(line), streampos});
                }
                break;
            }
        }

        BZ2_bzDecompressEnd(&strm);
    }
    else
    {
        std::wifstream stream(_file_name);
        if (stream.fail())
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _error_text = std::string("Could not open file '") + _file_name.string() + "'";
            _EOF        = true;
            _cv_not_empty.notify_all();
            _cv_not_full.notify_all();
            return;
        }

        stream.rdbuf()->pubsetbuf(nullptr, 1024 * 1024);

        std::streamoff streampos = 0;

        for (std::wstring line; std::getline(stream, line);)
        {
            put_line(Entry{std::move(line), streampos});
            streampos = stream.tellg();
        }
    }

    {
        std::lock_guard<std::mutex> lck(_mtx);
        _EOF = true;
    }
    _cv_not_empty.notify_all();
    _cv_not_full.notify_all();
}
