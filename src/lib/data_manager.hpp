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

#include "zelph.hpp"

#include <filesystem>
#include <memory>

namespace zelph::console
{
    enum class DataType
    {
        Generic,
        Wikidata
    };

    // An abstract strategy class responsible for populating a Zelph network from external files.
    // It handles file path resolution (e.g., finding a source file for a cache file), detects the
    // data schema (e.g., Wikidata vs. Generic), and orchestrates the import/loading process.
    // It serves as a factory to create the appropriate specific manager (Wikidata or Generic).
    class DataManager
    {
    public:
        virtual ~DataManager() = default;

        // Factory method: Determines the correct DataManager type based on the input path
        // and returns a shared_ptr to the specific instance (Wikidata or Generic).
        static std::shared_ptr<DataManager> create(network::Zelph* n, const std::filesystem::path& input_path);

        // Loads the data into the network (either from cache or source)
        virtual void load() = 0;

        // Optional: Set logging verbosity
        virtual void set_logging(bool do_log) {}

        virtual DataType get_type() const = 0;

    protected:
        DataManager(network::Zelph* n, const std::filesystem::path& input_path);

        // Helper to find the original source file (json/bz2) corresponding to the input.
        // If input is .bin and no source is found, returns an empty path.
        static std::filesystem::path resolve_original_source_path(const std::filesystem::path& input_path);

        network::Zelph*       _n;
        std::filesystem::path _input_path;
    };

    class GenericDataManager : public DataManager
    {
    public:
        GenericDataManager(network::Zelph* n, const std::filesystem::path& input_path);
        void     load() override;
        DataType get_type() const override { return DataType::Generic; }
    };
}
