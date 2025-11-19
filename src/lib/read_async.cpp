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
            _error_text = std::string("Could not open file '") + file_name.string() + "' to get size";
            _EOF        = true;
        }
    }

    ~Impl()
    {
        if (_t.joinable()) _t.join();
    }

    void read_thread(std::filesystem::path file_name);
    void put_line(const Entry& entry);

    size_t                  _sufficient_size;
    std::thread             _t;
    std::queue<Entry>       _lines;
    std::atomic<bool>       _EOF{false};
    std::string             _error_text;
    std::mutex              _mtx, _mtx2;
    std::condition_variable _cv, _cv2;
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
    _pImpl->_cv.wait(lock, [&]
                     { return _pImpl->_lines.size() > 0 || _pImpl->_EOF; });

    if (_pImpl->_lines.size() == 0)
    {
        return false;
    }
    line      = _pImpl->_lines.front()._line;
    streampos = _pImpl->_lines.front()._streampos;
    std::lock_guard<std::mutex> lck(_pImpl->_mtx2);
    _pImpl->_lines.pop();
    _pImpl->_cv2.notify_one();
    return true;
}

void ReadAsync::Impl::put_line(const Entry& entry)
{
    bool done = false;

    do
    {
        {
            std::lock_guard<std::mutex> lck(_mtx);

            if (_lines.size() < _sufficient_size)
            {
                _lines.push(entry);
                done = true;
            }
        }

        if (!done)
        {
            std::unique_lock<std::mutex> lock(_mtx2);
            _cv2.wait(lock, [&]
                      { return _lines.size() < _sufficient_size; });
        }
    } while (!done);

    _cv.notify_one();
}

void ReadAsync::Impl::read_thread(std::filesystem::path file_name)
{
    std::wifstream stream(file_name);

    if (stream.fail())
    {
        std::lock_guard<std::mutex> lck(_mtx);
        _error_text = std::string("Could not open file '") + file_name.string() + "'";
        _EOF        = true;
        _cv.notify_one();
        return;
    }

    stream.rdbuf()->pubsetbuf(nullptr, 1024 * 1024);

    // ReSharper disable once CppDFAUnreadVariable
    std::streamoff streampos = 0;

    for (std::wstring line; std::getline(stream, line);)
    {
        put_line({line, streampos});
        // ReSharper disable once CppDFAUnusedValue
        streampos = stream.tellg();
    }

    std::lock_guard<std::mutex> lck(_mtx);
    _EOF = true;
    _cv.notify_one();
}
