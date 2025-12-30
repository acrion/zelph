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

#include <filesystem>
#include <iosfwd>
#include <string>

namespace zelph
{
    namespace console
    {
        class ReadAsync
        {
        public:
            ReadAsync(const std::filesystem::path& file_name, size_t sufficient_size = 2);
            ~ReadAsync();
            std::streamsize get_total_size() const;
            bool            get_line(std::wstring& line, std::streamoff& streampos) const;
            std::string     error_text() const;

        private:
            class Impl;
            Impl* const _pImpl; // must stay at top of members list because of initialization order
        };
    }
}
