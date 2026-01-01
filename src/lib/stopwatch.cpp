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

#include "stopwatch.hpp"

#include <chrono>

using namespace zelph::network;
namespace chrono = std::chrono;

class StopWatch::CImpl
{
public:
    chrono::high_resolution_clock::time_point _start;
    chrono::high_resolution_clock::time_point _stop;
};

StopWatch::StopWatch()
    : _pImpl(new CImpl)
{
}

StopWatch::StopWatch(const StopWatch& stopWatch)
    : _pImpl(new CImpl)
{
    *_pImpl = *stopWatch._pImpl; // memberwise copy of CImpl members (which include no pointers)
}

StopWatch& StopWatch::operator=(const StopWatch& stopWatch)
{
    *_pImpl = *stopWatch._pImpl; // memberwise copy of CImpl members (which include no pointers)
    return *this;
}

StopWatch::~StopWatch()
{
    delete _pImpl;
}

bool StopWatch::is_running() const
{
    return _pImpl->_stop == chrono::time_point<chrono::high_resolution_clock>::min();
}

void StopWatch::start()
{
    _pImpl->_start = chrono::high_resolution_clock::now();
    _pImpl->_stop  = chrono::time_point<chrono::high_resolution_clock>::min();
}

void StopWatch::stop()
{
    _pImpl->_stop = std::chrono::high_resolution_clock::now();
}

uint64_t StopWatch::duration() const
{
    auto currentTime = is_running() ? chrono::high_resolution_clock::now() : _pImpl->_stop;
    return chrono::duration_cast<chrono::milliseconds>(currentTime - _pImpl->_start).count();
}

std::string StopWatch::format() const
{
    double total_seconds = static_cast<double>(duration()) / 1000.0;

    long   hours             = static_cast<long>(total_seconds / 3600);
    double remaining_seconds = total_seconds - hours * 3600.0;
    long   minutes           = static_cast<long>(remaining_seconds / 60);
    double seconds           = remaining_seconds - minutes * 60.0;

    std::ostringstream oss;
    oss << hours << "h" << minutes << "m"
        << std::fixed << std::setprecision(3) << seconds << "s";
    return oss.str();
}