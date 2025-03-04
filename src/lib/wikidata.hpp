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

#pragma once

#include "utils.hpp"
#include "zelph.hpp"

#include <filesystem>
#include <iosfwd>

namespace zelph
{
    namespace console
    {
#if __cplusplus > 201402L
        namespace fs = std::filesystem;
#else
        namespace fs = std::experimental::filesystem;
#endif

        class Wikidata
        {
        public:
            Wikidata(network::Zelph* n, const fs::path& file_name);
            ~Wikidata();

            void import_all();
            void generate_index() const;
            void traverse(std::wstring start_entry = L"");
            void process_node(network::Node node, const std::string& lang);

        private:
            void process_entry(const std::wstring& line, const bool log = true, const size_t thread_index = 0);
            void process_name(const std::wstring& wikidata_name);
            void index_entry(const std::wstring& line, const std::streamoff streampos) const;

            class Impl;
            Impl* const _pImpl; // must stay at top of members list because of initialization order

            size_t _running{0};
        };
    }
}
