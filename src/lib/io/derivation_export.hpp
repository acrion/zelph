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

// Derivation export: what a run DERIVED, in one target-agnostic format.
//
// zelph used to write its reports itself -- MkDocs markdown with Wikidata
// URLs, and a CJK-compressed variant of the same lines for language-model
// training. Both put knowledge of a specific consumer into the engine, which
// is the one thing the engine is supposed not to have: it does not know
// about Wikidata, and it should not know about MkDocs either.
//
// What the engine DOES know is the derivation and the identity of the nodes
// in it. That is what this writes: JSON Lines, one object per derived fact
// or contradiction,
//
//   {"kind":"deduction","conclusion":[SEGMENT,...],"premises":[[SEGMENT,...],...]}
//
// where a SEGMENT is either a JSON string (literal text of the rendering,
// e.g. brackets and spacing) or an object
//
//   {"names":{"wikidata":"Q5","en":"human"}}
//
// naming one node in every language it is known by. Everything downstream --
// which name to display, which of them is a URL, whether P-names are
// italicised, which file a line belongs in -- is a decision about a target
// format and belongs to the converter, not here. dev_scripts/zelph-export-md
// is the converter that reproduces the previous MkDocs reports; a converter
// for tokenizer-friendly training data is the same twenty lines with a
// different output branch.
//
// JSON Lines rather than one JSON document on purpose: a run over a Wikidata
// dump produces millions of these, and a line-per-record file streams,
// appends, greps and splits, while a single array does none of that.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zelph
{
    namespace network
    {
        class Zelph;
    }

    namespace io
    {
        class DerivationExport
        {
        public:
            DerivationExport(const std::filesystem::path& file, network::Zelph* zelph);
            ~DerivationExport();

            DerivationExport(const DerivationExport&)            = delete;
            DerivationExport& operator=(const DerivationExport&) = delete;
            DerivationExport(DerivationExport&&)                 = delete;
            DerivationExport& operator=(DerivationExport&&)      = delete;

            // `kind` is "deduction" or "contradiction". The strings are the
            // MARKED renderings node_to_string produces (identifiers in
            // guillemets); this class turns the markers into node records
            // and writes the line. Thread-safe: the reasoner reports from
            // its worker threads.
            void add(const std::string&              kind,
                     const std::string&              conclusion,
                     const std::vector<std::string>& premises) const;

        private:
            class Impl;
            std::unique_ptr<Impl> _impl;
        };
    }
}
