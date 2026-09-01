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

#include "derivation_export.hpp"

#include "network/zelph.hpp"

#include <fstream>
#include <mutex>
#include <stdexcept>

namespace zelph::io
{
    namespace
    {
        const std::string open_marker  = "«";
        const std::string close_marker = "»";

        std::string json_string(const std::string& s)
        {
            std::string out = "\"";
            for (const unsigned char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20)
                    {
                        // The only characters JSON insists on escaping
                        // numerically. Everything above is UTF-8 and is
                        // copied through byte by byte.
                        static const char* hex = "0123456789abcdef";
                        out += "\\u00";
                        out += hex[(c >> 4) & 0xF];
                        out += hex[c & 0xF];
                    }
                    else
                    {
                        out += static_cast<char>(c);
                    }
                }
            }
            return out + "\"";
        }
    }

    class DerivationExport::Impl
    {
    public:
        Impl(const std::filesystem::path& file, network::Zelph* zelph)
            : _zelph(zelph)
            , _out(file, std::ios::binary | std::ios::trunc)
        {
            if (!_out.is_open())
                throw std::runtime_error("Cannot open export file: " + file.string());
            _languages = zelph->get_languages();
        }

        // Every name the node is known by, as a JSON object. This is the
        // whole Wikidata "integration" of the export: none. A node that
        // happens to carry a name in the language "wikidata" reports it
        // like any other, and what that means is the reader's business.
        std::string names_of(const network::Node node) const
        {
            std::string out   = "{";
            bool        first = true;
            for (const std::string& lang : _languages)
            {
                const std::string name = _zelph->get_name(node, lang, false);
                if (name.empty()) continue;
                if (!first) out += ",";
                out += json_string(lang) + ":" + json_string(name);
                first = false;
            }
            return out + "}";
        }

        // The rendering marks every leaf name with guillemets, and
        // mark_identifier guarantees that a marked name contains none of
        // its own -- so this split is exact, which is what makes the
        // segments trustworthy rather than a best-effort parse.
        std::string segments_of(const std::string& marked) const
        {
            std::string out   = "[";
            bool        first = true;

            const auto emit = [&](const std::string& piece)
            {
                if (piece.empty()) return;
                if (!first) out += ",";
                out += piece;
                first = false;
            };

            const auto emit_literal = [&](const std::string& text)
            {
                if (!text.empty()) emit(json_string(text));
            };

            std::size_t pos = 0;
            while (pos < marked.size())
            {
                const std::size_t open  = marked.find(open_marker, pos);
                const std::size_t quote = marked.find('"', pos);

                // mark_identifier quotes instead of marking when the name
                // itself contains a guillemet -- the one case in-band
                // marking cannot represent. Such a name is still a node
                // reference and must not degrade into text.
                if (quote != std::string::npos && (open == std::string::npos || quote < open))
                {
                    const std::size_t end = marked.find('"', quote + 1);
                    if (end == std::string::npos)
                    {
                        emit_literal(marked.substr(pos));
                        break;
                    }
                    emit_literal(marked.substr(pos, quote - pos));
                    emit(identifier(marked.substr(quote + 1, end - quote - 1)));
                    pos = end + 1;
                    continue;
                }

                if (open == std::string::npos)
                {
                    emit_literal(marked.substr(pos));
                    break;
                }

                emit_literal(marked.substr(pos, open - pos));

                const std::size_t content_start = open + open_marker.size();
                const std::size_t close         = marked.find(close_marker, content_start);
                if (close == std::string::npos)
                {
                    emit_literal(marked.substr(content_start));
                    break;
                }

                const std::string display = marked.substr(content_start, close - content_start);

                emit(identifier(display));
                pos = close + close_marker.size();
            }

            return out + "]";
        }

    private:
        // Resolve a rendered leaf back to its node. get_formatted_name
        // composes "<wikidata id> - <label>" when the graph carries Wikidata
        // identifiers alongside natural-language labels; undoing that
        // composition is the engine inverting its OWN formatting decision,
        // which is why it lives here and not in a converter.
        network::Node resolve(const std::string& display) const
        {
            if (const network::Node n = _zelph->get_node(display, _zelph->lang())) return n;

            const std::size_t sep = display.find(" - ");
            if (sep != std::string::npos)
                if (const network::Node n = _zelph->get_node(display.substr(0, sep), "wikidata")) return n;

            return _zelph->get_core_node(display);
        }

        // A leaf that resolves to nothing stays literal text -- an honest
        // outcome, and the only one available for a rendering that is not a
        // name. A core node is flagged as such: "!" or "~" is part of
        // zelph's own vocabulary, and a converter must not mistake it for a
        // domain identifier it could look up somewhere.
        std::string identifier(const std::string& display) const
        {
            const network::Node node = resolve(display);
            if (node == 0) return json_string(display);

            const std::string core = _zelph->get_core_name(node);
            if (!core.empty()) return "{\"core\":" + json_string(core) + "}";

            const std::string names = names_of(node);
            if (names == "{}") return json_string(display);

            return "{\"names\":" + names + "}";
        }

    public:
        network::Zelph* const    _zelph;
        std::vector<std::string> _languages;
        mutable std::mutex       _mtx;
        mutable std::ofstream    _out;
    };

    DerivationExport::DerivationExport(const std::filesystem::path& file, network::Zelph* zelph)
        : _impl(std::make_unique<Impl>(file, zelph))
    {
    }

    DerivationExport::~DerivationExport() = default;

    void DerivationExport::add(const std::string& kind, const std::string& conclusion, const std::vector<std::string>& premises, const std::string& reason) const
    {
        std::string line = "{\"kind\":" + json_string(kind)
                         + ",\"conclusion\":" + _impl->segments_of(conclusion);

        if (!reason.empty()) line += ",\"refused\":" + json_string(reason);

        line += ",\"premises\":[";

        for (std::size_t i = 0; i < premises.size(); ++i)
        {
            if (i > 0) line += ",";
            line += _impl->segments_of(premises[i]);
        }
        line += "]}";

        std::lock_guard lock(_impl->_mtx);
        _impl->_out << line << "\n";
    }
}
