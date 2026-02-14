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

#include "data_manager.hpp"
#include "wikidata.hpp"

namespace zelph::console
{
    DataManager::DataManager(network::Zelph* n, const std::filesystem::path& input_path)
        : _n(n)
        , _input_path(input_path)
    {
    }

    std::filesystem::path DataManager::resolve_original_source_path(const std::filesystem::path& input_path)
    {
        namespace fs = std::filesystem;

        if (input_path.empty())
        {
            throw std::runtime_error("Input path must not be empty");
        }

        // CASE 1: Input is NOT a cache file (.bin).
        // It must be the direct source file. We check for existence immediately.
        if (input_path.extension() != ".bin")
        {
            if (!fs::exists(input_path))
            {
                return {};
            }
            return input_path;
        }

        // CASE 2: Input IS a cache file (.bin).
        // We try to locate the corresponding source file, but it is optional.

        fs::path base = input_path;
        base.replace_extension(""); // Remove .bin extension

        // 2a. Check for uncompressed JSON
        fs::path candidate_json = base;
        if (candidate_json.extension() != ".json")
        {
            candidate_json.replace_extension(".json");
        }
        if (fs::exists(candidate_json)) return candidate_json;

        // 2b. Check for compressed JSON (.json.bz2)
        fs::path candidate_json_bz2 = candidate_json;
        candidate_json_bz2 += ".bz2";
        if (fs::exists(candidate_json_bz2)) return candidate_json_bz2;

        // 2c. Check for generic compressed file (.bz2)
        fs::path candidate_bz2 = base;
        candidate_bz2.replace_extension(".bz2");
        if (fs::exists(candidate_bz2)) return candidate_bz2;

        // Source not found.
        return {};
    }

    std::shared_ptr<DataManager> DataManager::create(network::Zelph* n, const std::filesystem::path& input_path)
    {
        if (!std::filesystem::exists(input_path) && input_path.extension() != ".bin") // .bin might strictly rely on source, handled later
        {
            throw std::runtime_error("File does not exist: " + input_path.string());
        }

        // Resolve the potential source file behind the input
        std::filesystem::path source_path = resolve_original_source_path(input_path);

        // Decision Logic:
        // 1. If we found a source file, and it looks like JSON or BZ2, it's Wikidata (currently the only import format supported).
        //    Note: Even if input was .bin, if we found a source .json, we assume Wikidata context is desired.
        if (!source_path.empty())
        {
            auto ext = source_path.extension();
            if (ext == ".json" || ext == ".bz2")
            {
                return std::make_shared<Wikidata>(n, input_path);
            }
        }

        // 2. If no source file was found, but input is .bin, we treat it as a generic saved network.
        if (input_path.extension() == ".bin")
        {
            return std::make_shared<GenericDataManager>(n, input_path);
        }

        // 3. Fallback / Unknown format
        // If we have a file that exists but isn't .bin and resolve_original returned empty (or something unknown),
        // we default to Generic, but Generic currently only supports .bin loading in this impl.
        // However, if source_path was valid but unknown extension, we might throw or assume generic.
        // Given current capabilities:
        return std::make_shared<GenericDataManager>(n, input_path);
    }

    // --- GenericDataManager Implementation ---

    GenericDataManager::GenericDataManager(network::Zelph* n, const std::filesystem::path& input_path)
        : DataManager(n, input_path)
    {
    }

    void GenericDataManager::load()
    {
        if (_input_path.extension() != ".bin")
        {
            throw std::runtime_error("Generic data manager currently only supports loading .bin files directly.");
        }

        _n->print(L"Loading network from generic file " + _input_path.wstring() + L"...", true);
        _n->load_from_file(_input_path.string());
        _n->print(L"Network loaded.", true);
    }
}
