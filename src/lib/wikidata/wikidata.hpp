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

#include "io/data_manager.hpp"
#include "network/zelph.hpp"

#include <filesystem>
#include <unordered_set>

namespace zelph::wikidata
{
    struct ImportThreadStats;
    struct QualifierImportCounters;

    class Wikidata : public io::DataManager
    {
    public:
        // input_path can be a raw source file (.json, .bz2) or a cache file (.bin)
        Wikidata(network::Zelph* n, const std::filesystem::path& input_path);
        ~Wikidata() override;

        void         load() override;
        void         import_all(const std::string& constraints_dir = "");
        void         set_logging(bool do_log) override;
        io::DataType get_type() const override { return io::DataType::Wikidata; }
        /**
         * @brief Extracts the exact JSON lines for the given Wikidata IDs
         *        (Q…) from the dump and writes them as <id>.json
         *        into the current working directory.
         *        No import, no cache, no network activity.
         */
        void export_entities(const std::vector<std::string>& entity_ids);

        /**
         * @brief Imports statement qualifiers from the original Wikidata JSON
         *        dump into the current network, materializing reified
         *        statement structures as ordinary nodes and facts in the
         *        "wikidata" language:
         *          <subject>   p:<P>         <statement node>
         *          <statement> ps:<P>        <main value>
         *          <statement> pq:<Pq>       <qualifier value>
         *          <statement> wikibase:rank wikibase:{Normal|Preferred|Deprecated}Rank
         *        Statement nodes are named by their Wikidata statement ID
         *        (which always contains '$' and therefore cannot collide
         *        with Q/P IDs). The RDF prefix is kept as part of the
         *        predicate node name so that direct-triple predicates (bare
         *        P279 etc.) and statement-layer predicates never share a
         *        node; otherwise transitive closures like wdt:P279+ would
         *        leak into statement nodes.
         *
         *        An empty property list imports all qualifiers; otherwise
         *        only qualifiers whose property is listed are imported, and
         *        a statement is only materialized if it contributes at least
         *        one matching qualifier.
         *
         *        The base network (Q/P items) should be loaded beforehand so
         *        subjects and entity values attach to existing nodes via
         *        their "wikidata" names. The import is idempotent (facts are
         *        content-addressed), so it can be re-run or extended
         *        incrementally.
         */
        void import_qualifiers(const std::vector<std::string>& qualifier_properties);

    private:
        void process_constraints(const std::string& line, std::string id_str, const std::string& dir);
        void process_entry(const std::string& line,
                           const std::string& additional_language_to_import,
                           bool               log,
                           const std::string& constraints_dir,
                           ImportThreadStats* diag = nullptr);
        void process_import(const std::string& line,
                            const std::string& id_str,
                            const std::string& additional_language_to_import,
                            bool               log,
                            size_t             id1,
                            ImportThreadStats* diag = nullptr);
        void process_qualifier_entry(const std::string&                     line,
                                     const std::unordered_set<std::string>& selected_qualifier_properties,
                                     const std::vector<std::string>&        screen_patterns,
                                     QualifierImportCounters&               counters);

        class Impl;
        Impl* const _pImpl; // must stay at top of members list because of initialization order
    };
}
