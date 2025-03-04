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

#include <filesystem>
#include <list>
#include <utility>

namespace zelph
{
    namespace network
    {
        class Zelph;
    }

    namespace wikidata
    {
        class Markdown
        {
        public:
            Markdown(const std::filesystem::path& base_directory, network::Zelph* const zelph);

            void add(const std::wstring& heading, const std::wstring& message) const;

        private:
            std::pair<std::list<std::string>, std::wstring> convert_to_md(const std::wstring& message) const;
            std::string                                     get_wikidata_id(const std::wstring& token, const std::string& lang) const;
            std::string                                     get_template(const std::string& id) const;

            std::filesystem::path _base_directory;
            network::Zelph* const _zelph;
        };
    }
}
