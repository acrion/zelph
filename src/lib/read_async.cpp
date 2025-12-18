/*
Copyright (c) 2025 acrion innovations GmbH
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

#include <atomic>
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
    {
        std::ifstream file(file_name, std::ifstream::ate | std::ifstream::binary);
        if (file.is_open())
        {
            _total_size = file.tellg();
            file.close();
            _t = std::thread(&Impl::read_thread, this, file_name);
        }
        else
        {
            std::lock_guard<std::mutex> lck(_mtx);
            _error_text = std::string("Could not open file '") + file_name.string() + "' to get size";
            _EOF        = true;
        }
    }

    ~Impl()
    {
        if (_t.joinable()) _t.join();
    }

    void read_thread(std::filesystem::path file_name);
    void put_line(Entry entry);

    size_t                  _sufficient_size;
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

void ReadAsync::Impl::read_thread(std::filesystem::path file_name)
{
    std::wifstream stream(file_name);
    if (stream.fail())
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _error_text = std::string("Could not open file '") + file_name.string() + "'";
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

    {
        std::lock_guard<std::mutex> lck(_mtx);
        _EOF = true;
    }
    _cv_not_empty.notify_all();
    _cv_not_full.notify_all();
}
