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

#include "data_manager.hpp"
#include "string_utils.hpp"
#include "zelph.hpp"

#include <filesystem>
#include <iosfwd>

namespace zelph
{
    namespace console
    {
        class Wikidata : public DataManager
        {
        public:
            // input_path can be a raw source file (.json, .bz2) or a cache file (.bin)
            Wikidata(network::Zelph* n, const std::filesystem::path& input_path);
            ~Wikidata();

            void     load() override;
            void     import_all(const std::string& constraints_dir = "");
            void     set_logging(bool do_log) override;
            DataType get_type() const override { return DataType::Wikidata; }

        private:
            void process_constraints(const std::wstring& line, std::wstring id_str, const std::string& dir);
            void process_entry(const std::wstring& line, const std::string& additional_language_to_import, const bool log, const std::string& constraints_dir);
            void process_import(const std::wstring& line, const std::wstring& id_str, const std::string& additional_language_to_import, const bool log, size_t id1);

            class Impl;
            Impl* const _pImpl; // must stay at top of members list because of initialization order
        };
    }
}
