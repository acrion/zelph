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

#include <zelph_export.h>

#include <functional>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zelph::io
{
    enum class OutputChannel
    {
        Out,
        Error,
        Diagnostic,
        Prompt
    };

    struct OutputEvent
    {
        OutputChannel channel;
        std::string   text;
        bool          newline{true};
    };

    using OutputHandler = std::function<void(const OutputEvent&)>;

    ZELPH_EXPORT void default_output_handler(const OutputEvent& event);

    class OutputCollector
    {
    public:
        OutputHandler sink()
        {
            return [this](const OutputEvent& e)
            { handle(e); };
        }

        void handle(const OutputEvent& e)
        {
            _events.push_back(e);
        }

        const std::vector<OutputEvent>& events() const
        {
            return _events;
        }

        void clear()
        {
            _events.clear();
        }

    private:
        std::vector<OutputEvent> _events;
    };

    class OutputStream
    {
    public:
        using WManip   = std::ostream& (*)(std::ostream&);
        using IOSManip = std::ios_base& (*)(std::ios_base&);

        OutputStream(OutputHandler handler, OutputChannel channel, bool default_newline = true)
            : _handler(std::move(handler))
            , _channel(channel)
            , _default_newline(default_newline)
        {
        }

        OutputStream(const OutputStream&)            = delete;
        OutputStream& operator=(const OutputStream&) = delete;

        OutputStream(OutputStream&& other) noexcept
            : _handler(std::move(other._handler))
            , _channel(other._channel)
            , _default_newline(other._default_newline)
            , _stream(std::move(other._stream))
        {
            other._moved_from = true;
        }

        OutputStream& operator=(OutputStream&& other) noexcept
        {
            if (this != &other)
            {
                flush(_default_newline);

                _handler          = std::move(other._handler);
                _channel          = other._channel;
                _default_newline  = other._default_newline;
                _stream           = std::move(other._stream);
                _moved_from       = false;
                other._moved_from = true;
            }
            return *this;
        }

        ~OutputStream()
        {
            if (!_moved_from)
                flush(_default_newline);
        }

        template <typename T>
        OutputStream& operator<<(const T& value)
        {
            _stream << value;
            return *this;
        }

        OutputStream& operator<<(const std::string& value)
        {
            _stream << value;
            return *this;
        }

        OutputStream& operator<<(std::string_view value)
        {
            _stream << std::string(value);
            return *this;
        }

        OutputStream& operator<<(const char* value)
        {
            if (value)
                _stream << value;
            return *this;
        }

        OutputStream& operator<<(WManip manip)
        {
            if (manip == endl_manip())
            {
                flush(true);
                return *this;
            }
            if (manip == flush_manip())
            {
                flush(false);
                return *this;
            }

            manip(_stream);
            return *this;
        }

        OutputStream& operator<<(IOSManip manip)
        {
            manip(_stream);
            return *this;
        }

        void flush(bool newline = true)
        {
            if (!_handler)
                return;

            std::string text = _stream.str();

            if (text.empty() && !newline)
                return;

            _handler(OutputEvent{_channel, text, newline});

            _stream.str("");
            _stream.clear();
        }

    private:
        static WManip endl_manip()
        {
            return static_cast<WManip>(std::endl<char, std::char_traits<char>>);
        }

        static WManip flush_manip()
        {
            return static_cast<WManip>(std::flush<char, std::char_traits<char>>);
        }

        OutputHandler      _handler;
        OutputChannel      _channel;
        bool               _default_newline{true};
        bool               _moved_from{false};
        std::ostringstream _stream;
    };
}