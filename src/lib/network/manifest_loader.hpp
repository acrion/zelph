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

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "zelph.hpp"

namespace zelph::network
{
    class Impl;

    namespace detail
    {
        using chunk_selector = std::unordered_set<uint32_t>;

        struct ManifestChunkRef
        {
            uint32_t    chunk_index       = 0;
            uint64_t    source_offset     = 0;
            uint64_t    length            = 0;
            bool        has_source_offset = false;
            std::string object_path;
            std::string which;
            std::string lang;
        };

        struct ManifestSection
        {
            std::vector<ManifestChunkRef> chunks;
        };

        struct ManifestDescription
        {
            bool            is_v2                = false;
            bool            is_v3                = false;
            bool            node_route_supported = false;
            std::string     source_bin_path;
            std::string     node_route_index_path;
            std::string     node_route_index_local_path;
            uint64_t        source_header_length_bytes = 0;
            ManifestSection left;
            ManifestSection right;
            ManifestSection name_of_node;
            ManifestSection node_of_name;
        };

        struct RouteSelectionResolution
        {
            chunk_selector left;
            chunk_selector right;
            chunk_selector name_of_node;
            chunk_selector node_of_name;
            bool           any_match = false;
        };

        // --- Predicate index persistence ------------------------------------
        // Indexes are cached on disk next to the loaded .bin file
        // (<bin>.pidx.<predicate-node-id>) so that later sessions on the same
        // file skip the expensive extraction phase entirely. A sidecar is only
        // read or written while the in-memory graph is an unmodified image of
        // the loaded file: any edge mutation (everything that triggers
        // invalidate_fact_structures_cache, including pattern facts created by
        // queries through the unification path) disables sidecar I/O for the
        // rest of the session, and closures fall back to building from the
        // graph. The validation snapshot is taken at load time, so node
        // creation without edges (e.g. zelph/resolve during queries) does not
        // interfere. Files store host-endian raw pairs - they are machine-
        // local caches, not an interchange format.

        struct PidxHeader
        {
            char     magic[4]; // "ZPIX"
            uint32_t version;  // 1
            uint64_t predicate;
            uint64_t node_count;
            uint64_t last;
            uint64_t last_var;
            uint64_t pair_count;
        };

        using IndexPair = std::pair<Node, Node>; // (subject, object)
        static_assert(sizeof(IndexPair) == 2 * sizeof(Node), "IndexPair must be tightly packed");

        inline size_t skip_json_ws(std::string_view s, size_t pos)
        {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
            {
                ++pos;
            }
            return pos;
        }

        inline size_t find_json_key_position(std::string_view text, const std::string& key)
        {
            const std::string marker     = '"' + key + '"';
            size_t            search_pos = 0;
            while (search_pos < text.size())
            {
                const size_t key_pos = text.find(marker, search_pos);
                if (key_pos == std::string_view::npos)
                {
                    return std::string_view::npos;
                }

                bool in_string = false;
                bool escaped   = false;
                for (size_t i = 0; i < key_pos; ++i)
                {
                    const char c = text[i];
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (c == '\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (c == '"')
                    {
                        in_string = !in_string;
                    }
                }

                search_pos = key_pos + marker.size();
                if (in_string)
                {
                    continue;
                }

                const size_t colon_pos = skip_json_ws(text, key_pos + marker.size());
                if (colon_pos == std::string_view::npos || colon_pos >= text.size() || text[colon_pos] != ':')
                {
                    continue;
                }

                const size_t value_pos = skip_json_ws(text, colon_pos + 1);
                if (value_pos < text.size() && value_pos > colon_pos)
                {
                    return key_pos;
                }
            }

            return std::string_view::npos;
        }

        inline size_t parse_balanced_span(std::string_view text, size_t start_pos, char open, char close)
        {
            if (start_pos >= text.size() || text[start_pos] != open)
            {
                return std::string_view::npos;
            }

            int  depth     = 0;
            bool in_string = false;
            bool escaped   = false;
            for (size_t i = start_pos; i < text.size(); ++i)
            {
                char c = text[i];

                if (in_string)
                {
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (c == '\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (c == '"')
                    {
                        in_string = false;
                    }
                    continue;
                }

                if (c == '"')
                {
                    in_string = true;
                    continue;
                }

                if (c == open)
                {
                    ++depth;
                    continue;
                }

                if (c == close)
                {
                    --depth;
                    if (depth == 0)
                    {
                        return i + 1;
                    }
                }
            }

            return std::string_view::npos;
        }

        inline std::string_view extract_balanced(std::string_view text, size_t start_pos, char open, char close, size_t& next_pos)
        {
            const size_t span_end = parse_balanced_span(text, start_pos, open, close);
            if (span_end == std::string_view::npos)
            {
                next_pos = std::string_view::npos;
                return {};
            }

            next_pos = span_end;
            if (text[start_pos] == open)
            {
                return text.substr(start_pos, span_end - start_pos);
            }

            return {};
        }

        inline std::string_view find_json_object(std::string_view text, const std::string& key)
        {
            const std::string key_marker = '"' + key + '"';
            const size_t      key_pos    = find_json_key_position(text, key);
            if (key_pos == std::string_view::npos)
            {
                return {};
            }

            size_t colon_pos = text.find(':', key_pos + key_marker.size());
            if (colon_pos == std::string_view::npos)
            {
                return {};
            }

            size_t value_start = skip_json_ws(text, colon_pos + 1);
            if (value_start >= text.size() || text[value_start] != '{')
            {
                return {};
            }

            size_t end = 0;
            return extract_balanced(text, value_start, '{', '}', end);
        }

        inline chunk_selector make_chunk_selector(const std::vector<uint32_t>& indices)
        {
            return chunk_selector(indices.begin(), indices.end());
        }

        inline chunk_selector normalize_chunk_selector(const std::vector<uint32_t>& indices, uint32_t chunk_count, const bool explicit_selection)
        {
            if (!explicit_selection)
            {
                return {};
            }
            if (indices.empty())
            {
                return {};
            }
            return make_chunk_selector(indices);
        }

        inline bool is_hf_uri(const std::string& source)
        {
            return source.rfind("hf://", 0) == 0 || source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
        }

        inline std::string quote_shell_token(const std::string& value)
        {
            std::string out = "'";
            for (char c : value)
            {
                if (c == '\'')
                {
                    out += "'\"'\"'";
                }
                else
                {
                    out.push_back(c);
                }
            }
            out += "'";
            return out;
        }

        inline std::string hf_path_to_http_url(const std::string& hf_path)
        {
            if (hf_path.rfind("http://", 0) == 0 || hf_path.rfind("https://", 0) == 0)
            {
                return hf_path;
            }

            if (hf_path.rfind("hf://", 0) != 0)
            {
                return hf_path;
            }

            const std::string relative    = hf_path.substr(5);
            const size_t      first_slash = relative.find('/');
            if (first_slash == std::string::npos)
            {
                return "https://huggingface.co/" + relative;
            }

            const std::string kind        = relative.substr(0, first_slash);
            const std::string remainder   = relative.substr(first_slash + 1);
            const size_t      owner_slash = remainder.find('/');
            if (owner_slash == std::string::npos)
            {
                return "https://huggingface.co/" + relative;
            }

            const size_t repo_slash = remainder.find('/', owner_slash + 1);
            if (repo_slash == std::string::npos)
            {
                return "https://huggingface.co/" + relative;
            }

            const std::string repo_ref  = remainder.substr(0, repo_slash);
            const std::string file_path = remainder.substr(repo_slash + 1);

            if (kind == "datasets" || kind == "spaces")
            {
                return "https://huggingface.co/" + kind + "/" + repo_ref + "/resolve/main/" + file_path;
            }
            if (kind == "models")
            {
                return "https://huggingface.co/" + repo_ref + "/resolve/main/" + file_path;
            }

            return "https://huggingface.co/" + relative;
        }

        inline void run_shell_command(const std::string& cmd)
        {
            const int rc = std::system(cmd.c_str());
            if (rc != 0)
            {
                throw std::runtime_error("Command failed with code " + std::to_string(rc) + ": " + cmd);
            }
        }

        inline bool parse_json_number_field(std::string_view object_json, const std::string& key, uint64_t& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos                          = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size() || !std::isdigit(static_cast<unsigned char>(object_json[pos])))
            {
                return false;
            }

            size_t end = pos;
            while (end < object_json.size() && std::isdigit(static_cast<unsigned char>(object_json[end])))
            {
                ++end;
            }

            try
            {
                out = std::stoull(std::string(object_json.substr(pos, end - pos)));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        inline bool parse_json_string_field(std::string_view object_json, const std::string& key, std::string& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos                          = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size() || object_json[pos] != '"')
            {
                return false;
            }

            ++pos;
            std::string result;
            bool        escaped = false;
            while (pos < object_json.size())
            {
                char c = object_json[pos++];
                if (escaped)
                {
                    switch (c)
                    {
                    case '"':
                        result.push_back('"');
                        break;
                    case '\\':
                        result.push_back('\\');
                        break;
                    case '/':
                        result.push_back('/');
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u':
                        if (pos + 3 < object_json.size())
                        {
                            const std::string hex = std::string(object_json.substr(pos, 4));
                            if (std::isxdigit(static_cast<unsigned char>(hex[0])) && std::isxdigit(static_cast<unsigned char>(hex[1]))
                                && std::isxdigit(static_cast<unsigned char>(hex[2])) && std::isxdigit(static_cast<unsigned char>(hex[3])))
                            {
                                pos += 4;
                                result.push_back('?');
                                break;
                            }
                        }
                        return false;
                    default:
                        result.push_back(c);
                        break;
                    }
                    escaped = false;
                    continue;
                }

                if (c == '\\')
                {
                    escaped = true;
                    continue;
                }

                if (c == '"')
                {
                    out = std::move(result);
                    return true;
                }
                result.push_back(c);
            }

            return false;
        }

        inline bool parse_json_bool_field(std::string_view object_json, const std::string& key, bool& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos                          = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size())
            {
                return false;
            }

            if (object_json.substr(pos, 4) == "true")
            {
                out = true;
                return true;
            }
            if (object_json.substr(pos, 5) == "false")
            {
                out = false;
                return true;
            }
            return false;
        }

        inline bool parse_json_number_array_field(std::string_view object_json, const std::string& key, std::vector<uint64_t>& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos                          = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size() || object_json[pos] != '[')
            {
                return false;
            }

            size_t array_end  = 0;
            auto   array_json = extract_balanced(object_json, pos, '[', ']', array_end);
            if (array_json.empty())
            {
                return false;
            }

            out.clear();
            size_t       cursor = pos + 1;
            const size_t stop   = pos + array_json.size() - 1;
            while (cursor < stop)
            {
                cursor = skip_json_ws(object_json, cursor);
                if (cursor >= stop || object_json[cursor] == ']')
                {
                    break;
                }

                size_t end = cursor;
                while (end < stop && std::isdigit(static_cast<unsigned char>(object_json[end])))
                {
                    ++end;
                }
                if (end == cursor)
                {
                    return false;
                }

                try
                {
                    out.push_back(std::stoull(std::string(object_json.substr(cursor, end - cursor))));
                }
                catch (...)
                {
                    return false;
                }

                cursor = skip_json_ws(object_json, end);
                if (cursor < stop && object_json[cursor] == ',')
                {
                    ++cursor;
                }
            }

            return true;
        }

        inline bool parse_json_string_array_field(std::string_view object_json, const std::string& key, std::vector<std::string>& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos                          = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size() || object_json[pos] != '[')
            {
                return false;
            }

            size_t array_end  = 0;
            auto   array_json = extract_balanced(object_json, pos, '[', ']', array_end);
            if (array_json.empty())
            {
                return false;
            }

            out.clear();
            size_t       cursor = pos + 1;
            const size_t stop   = pos + array_json.size() - 1;
            while (cursor < stop)
            {
                cursor = skip_json_ws(object_json, cursor);
                if (cursor >= stop || object_json[cursor] == ']')
                {
                    break;
                }
                if (object_json[cursor] != '"')
                {
                    return false;
                }
                ++cursor;
                std::string value;
                bool        escaped = false;
                while (cursor < stop)
                {
                    char c = object_json[cursor++];
                    if (escaped)
                    {
                        switch (c)
                        {
                        case '"':
                            value.push_back('"');
                            break;
                        case '\\':
                            value.push_back('\\');
                            break;
                        case '/':
                            value.push_back('/');
                            break;
                        case 'b':
                            value.push_back('\b');
                            break;
                        case 'f':
                            value.push_back('\f');
                            break;
                        case 'n':
                            value.push_back('\n');
                            break;
                        case 'r':
                            value.push_back('\r');
                            break;
                        case 't':
                            value.push_back('\t');
                            break;
                        case 'u':
                            if (cursor + 3 <= stop)
                            {
                                cursor += 4;
                                value.push_back('?');
                                break;
                            }
                            return false;
                        default:
                            value.push_back(c);
                            break;
                        }
                        escaped = false;
                        continue;
                    }
                    if (c == '\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (c == '"')
                    {
                        break;
                    }
                    value.push_back(c);
                }
                out.push_back(std::move(value));

                cursor = skip_json_ws(object_json, cursor);
                if (cursor < stop && object_json[cursor] == ',')
                {
                    ++cursor;
                }
            }

            return true;
        }

        inline ManifestSection parse_section_chunks(std::string_view section_object, const std::string& section_name)
        {
            const auto section_obj_view = find_json_object(section_object, section_name);
            if (section_obj_view.empty())
            {
                throw std::runtime_error("Manifest section missing: " + section_name);
            }

            const std::string chunks_key     = "\"chunks\"";
            const size_t      chunks_key_pos = section_obj_view.find(chunks_key);
            if (chunks_key_pos == std::string_view::npos)
            {
                throw std::runtime_error("Manifest section malformed (missing array for section): " + section_name);
            }

            size_t colon_pos   = section_obj_view.find(':', chunks_key_pos + chunks_key.size());
            size_t array_start = skip_json_ws(section_obj_view, colon_pos + 1);

            if (array_start == std::string_view::npos || array_start >= section_obj_view.size() || section_obj_view[array_start] != '[')
            {
                throw std::runtime_error("Manifest section malformed (missing '['): " + section_name);
            }

            size_t array_end     = 0;
            auto   section_array = extract_balanced(section_obj_view, array_start, '[', ']', array_end);
            if (section_array.empty())
            {
                throw std::runtime_error("Manifest section malformed (unclosed '['): " + section_name);
            }

            ManifestSection section;
            size_t          cursor     = array_start + 1;
            const size_t    array_stop = array_start + section_array.size() - 1;
            while (cursor < array_stop)
            {
                cursor = skip_json_ws(section_obj_view, cursor);
                if (cursor >= array_stop || section_obj_view[cursor] == ']')
                {
                    break;
                }
                if (section_obj_view[cursor] != '{')
                {
                    throw std::runtime_error("Manifest section malformed (expected object): " + section_name);
                }

                size_t obj_end     = 0;
                auto   object_json = extract_balanced(section_obj_view, cursor, '{', '}', obj_end);
                if (object_json.empty())
                {
                    throw std::runtime_error("Manifest section malformed (chunk object parse): " + section_name);
                }

                ManifestChunkRef chunk_ref;
                uint64_t         chunk_index = 0;

                if (!parse_json_number_field(object_json, "chunkIndex", chunk_index))
                {
                    throw std::runtime_error("Manifest chunk missing chunkIndex in section " + section_name);
                }
                chunk_ref.chunk_index = static_cast<uint32_t>(chunk_index);

                if (!parse_json_number_field(object_json, "sourceOffset", chunk_ref.source_offset))
                {
                    chunk_ref.has_source_offset = parse_json_number_field(object_json, "offset", chunk_ref.source_offset);
                }
                else
                {
                    chunk_ref.has_source_offset = true;
                }

                if (!parse_json_number_field(object_json, "length", chunk_ref.length))
                {
                    throw std::runtime_error("Manifest chunk missing length in section " + section_name);
                }

                parse_json_string_field(object_json, "objectPath", chunk_ref.object_path);
                if (chunk_ref.object_path.empty())
                {
                    parse_json_string_field(object_json, "futureObjectPath", chunk_ref.object_path);
                }
                parse_json_string_field(object_json, "which", chunk_ref.which);
                parse_json_string_field(object_json, "lang", chunk_ref.lang);
                section.chunks.push_back(std::move(chunk_ref));

                cursor = obj_end;
                cursor = skip_json_ws(section_obj_view, cursor);
                if (cursor < array_stop && section_obj_view[cursor] == ',')
                {
                    ++cursor;
                }
            }

            return section;
        }

        inline ManifestDescription parse_manifest_file(const std::string& manifest_path)
        {
            std::ifstream in(manifest_path);
            if (!in.is_open())
            {
                throw std::runtime_error("Cannot open manifest file: " + manifest_path);
            }

            std::string json_text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (json_text.empty())
            {
                throw std::runtime_error("Manifest file is empty: " + manifest_path);
            }

            ManifestDescription manifest;
            manifest.is_v2 = json_text.find("zelph-hf-layout/v2") != std::string::npos;
            manifest.is_v3 = json_text.find("zelph-hf-layout/v3") != std::string::npos;

            auto json_view  = std::string_view(json_text);
            auto source_obj = find_json_object(json_view, "source");

            if (!source_obj.empty())
            {
                if (!parse_json_string_field(source_obj, "binPath", manifest.source_bin_path))
                {
                    parse_json_string_field(source_obj, "path", manifest.source_bin_path);
                }
                if (!parse_json_number_field(source_obj, "headerLengthBytes", manifest.source_header_length_bytes))
                {
                    parse_json_number_field(source_obj, "headerLength", manifest.source_header_length_bytes);
                    parse_json_number_field(source_obj, "header_length_bytes", manifest.source_header_length_bytes);
                }
            }

            if (manifest.source_bin_path.empty())
            {
                const auto hf_objects = find_json_object(json_view, "hfObjects");
                if (!hf_objects.empty())
                {
                    const auto bin_obj = find_json_object(hf_objects, "bin");
                    if (!bin_obj.empty())
                    {
                        parse_json_string_field(bin_obj, "path", manifest.source_bin_path);
                    }

                    const auto node_route_obj = find_json_object(hf_objects, "nodeRouteIndex");
                    if (!node_route_obj.empty())
                    {
                        parse_json_string_field(node_route_obj, "path", manifest.node_route_index_path);
                        parse_json_string_field(node_route_obj, "localPath", manifest.node_route_index_local_path);
                    }
                }
            }

            {
                const auto hf_objects = find_json_object(json_view, "hfObjects");
                if (!hf_objects.empty() && manifest.node_route_index_path.empty() && manifest.node_route_index_local_path.empty())
                {
                    const auto node_route_obj = find_json_object(hf_objects, "nodeRouteIndex");
                    if (!node_route_obj.empty())
                    {
                        parse_json_string_field(node_route_obj, "path", manifest.node_route_index_path);
                        parse_json_string_field(node_route_obj, "localPath", manifest.node_route_index_local_path);
                    }
                }
            }

            manifest.node_route_supported = json_text.find("\"node-route\"") != std::string::npos;
            if (!manifest.node_route_supported)
            {
                const auto capabilities_obj = find_json_object(json_view, "capabilities");
                if (!capabilities_obj.empty())
                {
                    parse_json_bool_field(capabilities_obj, "nodeRouteIndex", manifest.node_route_supported);
                }
            }
            if (!manifest.node_route_supported)
            {
                const auto layout_plan_obj = find_json_object(json_view, "layoutPlan");
                if (!layout_plan_obj.empty())
                {
                    parse_json_bool_field(layout_plan_obj, "supportsNodeRouteIndex", manifest.node_route_supported);
                }
            }

            auto sections_obj = find_json_object(json_view, "sections");
            if (sections_obj.empty())
            {
                throw std::runtime_error("Manifest missing sections object: " + manifest_path);
            }

            manifest.left         = parse_section_chunks(sections_obj, "left");
            manifest.right        = parse_section_chunks(sections_obj, "right");
            manifest.name_of_node = parse_section_chunks(sections_obj, "nameOfNode");
            manifest.node_of_name = parse_section_chunks(sections_obj, "nodeOfName");

            return manifest;
        }

        inline std::filesystem::path resolve_manifest_chunk_path(const std::string& manifest_path,
                                                                 const std::string& object_path,
                                                                 const std::string& shard_root)
        {
            namespace fs = std::filesystem;

            const fs::path manifest_dir = fs::path(manifest_path).parent_path();
            std::string    normalized   = object_path;

            if (normalized.rfind("hf://", 0) == 0)
            {
                normalized = normalized.substr(5);
                if (!normalized.empty() && normalized.front() == '/')
                {
                    normalized.erase(0, 1);
                }
            }

            const fs::path        local_obj_path = fs::path(normalized);
            std::vector<fs::path> candidates     = {local_obj_path, manifest_dir / local_obj_path};

            if (!shard_root.empty())
            {
                fs::path shard_base{shard_root};
                candidates.emplace_back(shard_base / local_obj_path);

                const std::string shards_marker = "/shards/";
                if (auto pos = normalized.find(shards_marker); pos != std::string::npos)
                {
                    const std::string tail = normalized.substr(pos + shards_marker.size());
                    candidates.emplace_back(shard_base / tail);
                }

                const std::string chunks_marker = "/chunks/";
                if (auto pos = normalized.find(chunks_marker); pos != std::string::npos)
                {
                    const std::string tail = normalized.substr(pos + chunks_marker.size());
                    candidates.emplace_back(shard_base / tail);
                }

                const std::string hf_path_sep = "datasets/";
                if (auto pos = normalized.find(hf_path_sep); pos != std::string::npos)
                {
                    const std::string tail = normalized.substr(pos + hf_path_sep.size());
                    const fs::path    maybe_tail{tail};
                    if (!maybe_tail.empty())
                    {
                        const auto parent = maybe_tail.parent_path();
                        candidates.emplace_back(shard_base / parent.filename() / maybe_tail.filename());
                    }
                }

                if (std::string filename = fs::path(normalized).filename().string(); !filename.empty())
                {
                    candidates.emplace_back(shard_base / filename);
                }
            }

            for (const auto& candidate : candidates)
            {
                if (!candidate.empty() && fs::exists(candidate))
                {
                    return fs::absolute(candidate);
                }
            }

            throw std::runtime_error("Manifest chunk path not found: " + object_path
                                     + " (tried manifest directory and shard root "
                                     + (shard_root.empty() ? "<not set>" : shard_root) + ")");
        }

        inline std::filesystem::path resolve_manifest_local_path(const std::string& manifest_path,
                                                                 const std::string& local_path,
                                                                 const std::string& shard_root)
        {
            namespace fs = std::filesystem;

            if (local_path.empty())
            {
                return {};
            }

            const fs::path        direct{local_path};
            std::vector<fs::path> candidates = {direct, fs::path(manifest_path).parent_path() / direct};
            if (!shard_root.empty())
            {
                candidates.emplace_back(fs::path(shard_root) / direct.filename());
            }

            for (const auto& candidate : candidates)
            {
                if (!candidate.empty() && fs::exists(candidate))
                {
                    return fs::absolute(candidate);
                }
            }

            throw std::runtime_error("Manifest nodeRouteIndex localPath not found: " + local_path);
        }

        inline std::filesystem::path ensure_cache_dir()
        {
            namespace fs        = std::filesystem;
            fs::path cache_root = fs::temp_directory_path() / "zelph-hf-cache";
            if (!fs::exists(cache_root))
            {
                fs::create_directories(cache_root);
            }
            return cache_root;
        }

        inline std::string sanitize_cache_token(const std::string& in)
        {
            std::string token;
            token.reserve(in.size());
            for (char c : in)
            {
                const bool is_safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                                  || c == '-' || c == '_' || c == '.';
                token.push_back(is_safe ? c : '_');
            }

            if (token.empty())
            {
                token = "chunk";
            }
            return token;
        }

        inline std::filesystem::path fetch_chunk_to_cache(const std::string& source,
                                                          const uint64_t     offset,
                                                          const uint64_t     length,
                                                          const std::string& section_hint)
        {
            namespace fs = std::filesystem;

            std::string token      = sanitize_cache_token(section_hint);
            std::string src_token  = sanitize_cache_token(source);
            fs::path    cache_file = ensure_cache_dir() / fs::path(token + "_" + src_token + "_" + std::to_string(offset) + "_" + std::to_string(length) + ".capnp-packed");

            if (fs::exists(cache_file))
            {
                if (length == 0 || fs::file_size(cache_file) == length)
                {
                    return cache_file;
                }
            }

            if (!is_hf_uri(source) && !source.empty() && source.rfind("file://", 0) != 0)
            {
                throw std::runtime_error("Expected remote source for cached fetch: " + source);
            }

            const std::string url = hf_path_to_http_url(source);

            std::string range_arg;
            if (length > 0)
            {
                range_arg = " --range " + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
            }
            const std::string cmd = "curl -fsSL" + range_arg + " " + quote_shell_token(url) + " -o " + quote_shell_token(cache_file.string());
            run_shell_command(cmd);

            if (length > 0 && fs::file_size(cache_file) != length)
            {
                throw std::runtime_error("Downloaded chunk size mismatch for " + source + ", expected " + std::to_string(length)
                                         + ", got " + std::to_string(fs::file_size(cache_file)));
            }

            return cache_file;
        }

        inline std::filesystem::path resolve_node_route_index_path(const std::string&         manifest_path,
                                                                   const ManifestDescription& manifest,
                                                                   const std::string&         shard_root)
        {
            if (!manifest.node_route_index_local_path.empty())
            {
                return resolve_manifest_local_path(manifest_path, manifest.node_route_index_local_path, shard_root);
            }

            if (manifest.node_route_index_path.empty())
            {
                throw std::runtime_error("Manifest does not advertise nodeRouteIndex.path");
            }

            if (is_hf_uri(manifest.node_route_index_path))
            {
                if (!shard_root.empty())
                {
                    try
                    {
                        return resolve_manifest_chunk_path(manifest_path, manifest.node_route_index_path, shard_root);
                    }
                    catch (...)
                    {
                    }
                }
                return fetch_chunk_to_cache(manifest.node_route_index_path, 0, 0, "node-route-index");
            }

            return resolve_manifest_chunk_path(manifest_path, manifest.node_route_index_path, shard_root);
        }

        inline std::string_view find_json_array(std::string_view text, const std::string& key)
        {
            const std::string key_marker = '"' + key + '"';
            const size_t      key_pos    = find_json_key_position(text, key);
            if (key_pos == std::string_view::npos)
            {
                return {};
            }

            size_t colon_pos = text.find(':', key_pos + key_marker.size());
            if (colon_pos == std::string_view::npos)
            {
                return {};
            }

            size_t value_start = skip_json_ws(text, colon_pos + 1);
            if (value_start >= text.size() || text[value_start] != '[')
            {
                return {};
            }

            size_t end = 0;
            return extract_balanced(text, value_start, '[', ']', end);
        }

        inline bool array_contains_uint64(const std::vector<uint64_t>& values, uint64_t needle)
        {
            for (uint64_t value : values)
            {
                if (value == needle)
                {
                    return true;
                }
            }
            return false;
        }

        inline bool array_contains_string(const std::vector<std::string>& values, const std::string& needle)
        {
            for (const auto& value : values)
            {
                if (value == needle)
                {
                    return true;
                }
            }
            return false;
        }

        inline void collect_route_section_matches(std::string_view                             routing_obj,
                                                  const std::string&                           section_name,
                                                  const std::function<void(std::string_view)>& visitor)
        {
            const auto section_array = find_json_array(routing_obj, section_name);
            if (section_array.empty())
            {
                return;
            }

            const size_t array_start = routing_obj.find(section_array);
            size_t       cursor      = array_start + 1;
            const size_t stop        = array_start + section_array.size() - 1;

            while (cursor < stop)
            {
                cursor = skip_json_ws(routing_obj, cursor);
                if (cursor >= stop || routing_obj[cursor] == ']')
                {
                    break;
                }
                if (routing_obj[cursor] != '{')
                {
                    throw std::runtime_error("Malformed nodeRouteIndex section: " + section_name);
                }

                size_t obj_end     = 0;
                auto   object_json = extract_balanced(routing_obj, cursor, '{', '}', obj_end);
                if (object_json.empty())
                {
                    throw std::runtime_error("Malformed nodeRouteIndex entry in section: " + section_name);
                }

                visitor(object_json);

                cursor = skip_json_ws(routing_obj, obj_end);
                if (cursor < stop && routing_obj[cursor] == ',')
                {
                    ++cursor;
                }
            }
        }

        inline RouteSelectionResolution resolve_route_selection(const std::string&              manifest_path,
                                                                const ManifestDescription&      manifest,
                                                                const Zelph::BinChunkSelection& selection,
                                                                const std::string&              shard_root)
        {
            if (!manifest.node_route_supported && manifest.node_route_index_path.empty() && manifest.node_route_index_local_path.empty())
            {
                throw std::runtime_error("Manifest does not advertise nodeRouteIndex support");
            }

            const auto    route_path = resolve_node_route_index_path(manifest_path, manifest, shard_root);
            std::ifstream in(route_path);
            if (!in.is_open())
            {
                throw std::runtime_error("Cannot open nodeRouteIndex sidecar: " + route_path.string());
            }

            std::string json_text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (json_text.empty())
            {
                throw std::runtime_error("nodeRouteIndex sidecar is empty: " + route_path.string());
            }

            auto routing_obj = find_json_object(std::string_view(json_text), "routing");
            if (routing_obj.empty())
            {
                throw std::runtime_error("nodeRouteIndex sidecar missing routing object: " + route_path.string());
            }

            RouteSelectionResolution resolved;

            if (selection.route_nodes_explicit)
            {
                collect_route_section_matches(routing_obj, "left", [&](std::string_view object_json)
                                              {
                        uint64_t chunk_index = 0;
                        std::vector<uint64_t> nodes;
                        if (!parse_json_number_field(object_json, "chunkIndex", chunk_index)
                            || !parse_json_number_array_field(object_json, "nodes", nodes))
                        {
                            throw std::runtime_error("Malformed nodeRouteIndex left entry");
                        }
    
                        for (uint64_t route_node : selection.route_nodes)
                        {
                            if (array_contains_uint64(nodes, route_node))
                            {
                                resolved.left.insert(static_cast<uint32_t>(chunk_index));
                                resolved.any_match = true;
                                break;
                            }
                        } });

                collect_route_section_matches(routing_obj, "right", [&](std::string_view object_json)
                                              {
                        uint64_t chunk_index = 0;
                        std::vector<uint64_t> nodes;
                        if (!parse_json_number_field(object_json, "chunkIndex", chunk_index)
                            || !parse_json_number_array_field(object_json, "nodes", nodes))
                        {
                            throw std::runtime_error("Malformed nodeRouteIndex right entry");
                        }
    
                        for (uint64_t route_node : selection.route_nodes)
                        {
                            if (array_contains_uint64(nodes, route_node))
                            {
                                resolved.right.insert(static_cast<uint32_t>(chunk_index));
                                resolved.any_match = true;
                                break;
                            }
                        } });

                collect_route_section_matches(routing_obj, "nameOfNode", [&](std::string_view object_json)
                                              {
                        uint64_t chunk_index = 0;
                        std::vector<uint64_t> nodes;
                        if (!parse_json_number_field(object_json, "chunkIndex", chunk_index)
                            || !parse_json_number_array_field(object_json, "nodes", nodes))
                        {
                            throw std::runtime_error("Malformed nodeRouteIndex nameOfNode entry");
                        }
    
                        for (uint64_t route_node : selection.route_nodes)
                        {
                            if (array_contains_uint64(nodes, route_node))
                            {
                                resolved.name_of_node.insert(static_cast<uint32_t>(chunk_index));
                                resolved.any_match = true;
                                break;
                            }
                        } });
            }

            if (selection.route_name_explicit)
            {
                collect_route_section_matches(routing_obj, "nodeOfName", [&](std::string_view object_json)
                                              {
                        uint64_t chunk_index = 0;
                        std::string lang;
                        std::vector<std::string> names;
                        if (!parse_json_number_field(object_json, "chunkIndex", chunk_index)
                            || !parse_json_string_field(object_json, "lang", lang)
                            || !parse_json_string_array_field(object_json, "names", names))
                        {
                            throw std::runtime_error("Malformed nodeRouteIndex nodeOfName entry");
                        }
    
                        if (lang == selection.route_lang && array_contains_string(names, selection.route_name))
                        {
                            resolved.node_of_name.insert(static_cast<uint32_t>(chunk_index));
                            resolved.any_match = true;
                        } });
            }

            if (!resolved.any_match)
            {
                throw std::runtime_error("nodeRouteIndex resolved no matching chunks for requested route selectors");
            }

            return resolved;
        }

        inline FILE* open_file_or_throw(const std::string& path)
        {
            FILE* file = fopen(path.c_str(), "rb");
            if (!file)
            {
                throw std::runtime_error("Failed to open file for reading: " + path);
            }
            return file;
        }

        inline void seek_offset_or_throw(FILE* file, uint64_t offset)
        {
            if (offset == 0) return;
            if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
            {
                throw std::runtime_error("Failed to seek to chunk source offset");
            }
        }

    } // namespace detail
} // namespace zelph::network