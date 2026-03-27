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

#include "network.hpp"
#include "zelph.hpp"

#include "io/zelph.capnp.h"

#include <ankerl/unordered_dense.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <vector>
#include <cctype>

namespace zelph::network
{
    // Append-only string pool providing pointer-stable storage.
    // All name strings are interned here exactly once; both name maps
    // store std::string_view pointing into this pool.
    //
    // Thread safety: callers must hold at least one of the name mutexes
    // (_mtx_name_of_node or _mtx_node_of_name) before calling intern().
    // In practice every code path that interns already holds both.
    class StringPool
    {
        // std::unordered_set is node-based => pointers/references to
        // elements are never invalidated by insert or rehash.
        std::unordered_set<std::string> _pool;

    public:
        // Returns a view whose lifetime equals the pool's lifetime
        // (or until clear() is called).
        std::string_view intern(const std::string& s)
        {
            auto [it, _] = _pool.insert(s);
            return std::string_view(*it);
        }

        std::string_view intern(std::string&& s)
        {
            auto [it, _] = _pool.insert(std::move(s));
            return std::string_view(*it);
        }

        void   clear() { _pool.clear(); }
        size_t size() const { return _pool.size(); }
    };

    class ZELPH_EXPORT Zelph::Impl : public Network
    {
        friend class Zelph;

        explicit Impl(const io::OutputHandler& output)
            : _output(output)
        {
        }

        using chunk_selector = std::unordered_set<uint32_t>;

        static chunk_selector make_chunk_selector(const std::vector<uint32_t>& indices)
        {
            return chunk_selector(indices.begin(), indices.end());
        }

        static chunk_selector make_all_chunk_selector(uint32_t chunk_count)
        {
            chunk_selector result;
            for (uint32_t i = 0; i < chunk_count; ++i)
            {
                result.insert(i);
            }
            return result;
        }

        static chunk_selector normalize_chunk_selector(const std::vector<uint32_t>& indices, uint32_t chunk_count, const bool explicit_selection)
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

        static bool is_hf_uri(const std::string& source)
        {
            return source.rfind("hf://", 0) == 0 || source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0;
        }

        static std::string quote_shell_token(const std::string& value)
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

        static std::string hf_path_to_http_url(const std::string& hf_path)
        {
            if (hf_path.rfind("http://", 0) == 0 || hf_path.rfind("https://", 0) == 0)
            {
                return hf_path;
            }

            if (hf_path.rfind("hf://", 0) != 0)
            {
                return hf_path;
            }

            const std::string relative = hf_path.substr(5);
            const size_t first_slash = relative.find('/');
            if (first_slash == std::string::npos)
            {
                return "https://huggingface.co/" + relative;
            }

            const std::string kind = relative.substr(0, first_slash);
            const std::string remainder = relative.substr(first_slash + 1);
            const size_t owner_slash = remainder.find('/');
            if (owner_slash == std::string::npos)
            {
                return "https://huggingface.co/" + relative;
            }

            const size_t repo_slash = remainder.find('/', owner_slash + 1);
            if (repo_slash == std::string::npos)
            {
                return "https://huggingface.co/" + relative;
            }

            const std::string repo_ref = remainder.substr(0, repo_slash);
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

        static void run_shell_command(const std::string& cmd)
        {
            const int rc = std::system(cmd.c_str());
            if (rc != 0)
            {
                throw std::runtime_error("Command failed with code " + std::to_string(rc) + ": " + cmd);
            }
        }

        static void validate_chunk_selector(const chunk_selector& selection, uint32_t chunkCount, const char* label)
        {
            for (uint32_t index : selection)
            {
                if (index >= chunkCount)
                {
                    throw std::runtime_error(std::string("Requested ")
                                             + label
                                             + " chunk "
                                             + std::to_string(index)
                                             + " but the file only has "
                                             + std::to_string(chunkCount)
                                             + " chunks");
                }
            }
        }

        void clear_loaded_state()
        {
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
            std::unique_lock<std::shared_mutex> lock_prob(_mtx_prob);
            std::unique_lock<std::shared_mutex> lock_node_name(_mtx_node_of_name);
            std::unique_lock<std::shared_mutex> lock_name_node(_mtx_name_of_node);

            _left.clear();
            _right.clear();
            _probabilities.clear();
            _name_of_node.clear();
            _node_of_name.clear();
            _string_pool.clear();
        }

        void loadSmallData(const ZelphImpl::Reader& impl)
        {
#ifdef CLEAR_ON_LOAD
            _probabilities.clear();
#endif
            for (auto p : impl.getProbabilities())
            {
                _probabilities[p.getHash()] = static_cast<long double>(p.getProb());
            }
            _last     = impl.getLast();
            _last_var = impl.getLastVar();
        }

        void loadLeftRightChunks(kj::BufferedInputStreamWrapper&       bufferedInput,
                                 const ::capnp::ReaderOptions&         options,
                                 uint32_t                              leftChunkCount,
                                 uint32_t                              rightChunkCount,
                                 const chunk_selector*                 leftSelection  = nullptr,
                                 const chunk_selector*                 rightSelection = nullptr)
        {
#ifdef CLEAR_ON_LOAD
            _left.clear();
#endif
            for (uint32_t chunkIdx = 0; chunkIdx < leftChunkCount; ++chunkIdx)
            {
                ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
                auto                         chunk = chunkMessage.getRoot<AdjChunk>();
                if (chunk.getWhich() != "left" || chunk.getChunkIndex() != chunkIdx)
                {
                    throw std::runtime_error("Invalid left chunk order");
                }
                const bool shouldLoad = leftSelection == nullptr || leftSelection->count(chunkIdx) == 1;
                if (shouldLoad)
                {
                    for (auto pair : chunk.getPairs())
                    {
                        adjacency_set adj;
                        for (auto n : pair.getAdj())
                        {
                            adj.insert(n);
                        }
                        _left[pair.getNode()] = std::move(adj);
                    }
                }
#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded left chunk " << chunkIdx + 1 << "/" << leftChunkCount << ", current _left size=" << _left.size();
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif
            }
#ifdef NDEBUG
            io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
#endif

#ifdef CLEAR_ON_LOAD
            _right.clear();
#endif
            for (uint32_t chunkIdx = 0; chunkIdx < rightChunkCount; ++chunkIdx)
            {
                ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
                auto                         chunk = chunkMessage.getRoot<AdjChunk>();
                if (chunk.getWhich() != "right" || chunk.getChunkIndex() != chunkIdx)
                {
                    throw std::runtime_error("Invalid right chunk order");
                }
                const bool shouldLoad = rightSelection == nullptr || rightSelection->count(chunkIdx) == 1;
                if (shouldLoad)
                {
                    for (auto pair : chunk.getPairs())
                    {
                        adjacency_set adj;
                        for (auto n : pair.getAdj())
                        {
                            adj.insert(n);
                        }
                        _right[pair.getNode()] = std::move(adj);
                    }
                }
#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded right chunk " << chunkIdx + 1 << "/" << rightChunkCount << ", current _right size=" << _right.size();
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif
            }
#ifdef NDEBUG
            io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
#endif
        }

        void loadNameOfNodeChunks(kj::BufferedInputStreamWrapper& bufferedInput,
                                  const ::capnp::ReaderOptions&  options,
                                  uint32_t                       nameOfNodeChunkCount,
                                  const chunk_selector*          selection = nullptr)
        {
#ifdef CLEAR_ON_LOAD
            _name_of_node.clear();
            _string_pool.clear();
#endif
            for (uint32_t i = 0; i < nameOfNodeChunkCount; ++i)
            {
                ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
                auto                         chunk      = chunkMessage.getRoot<NameChunk>();
                const bool                   shouldLoad = selection == nullptr || selection->count(i) == 1;
                if (shouldLoad)
                {
                    std::string lang = chunk.getLang();
                    auto&       map  = _name_of_node[lang];
                    for (auto pair : chunk.getPairs())
                    {
                        try
                        {
                            std::string_view sv = _string_pool.intern(pair.getValue());
                            map[pair.getKey()]  = sv;
                        }
                        catch (...)
                        {
                            std::string_view sv = _string_pool.intern("?");
                            map[pair.getKey()]  = sv;
                            io::OutputStream(_output, io::OutputChannel::Error, true) << "Error converting UTF-8 to string for name_of_node key " << pair.getKey();
                        }
                    }
                }
#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded name_of_node chunk " << i + 1 << "/" << nameOfNodeChunkCount;
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif
            }
#ifdef NDEBUG
            io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
#endif
        }

        void loadNodeOfNameChunks(kj::BufferedInputStreamWrapper& bufferedInput,
                                  const ::capnp::ReaderOptions&  options,
                                  uint32_t                       nodeOfNameChunkCount,
                                  const chunk_selector*          selection = nullptr)
        {
#ifdef CLEAR_ON_LOAD
            _node_of_name.clear();
#endif
            for (uint32_t i = 0; i < nodeOfNameChunkCount; ++i)
            {
                ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
                auto                         chunk      = chunkMessage.getRoot<NodeNameChunk>();
                const bool                   shouldLoad = selection == nullptr || selection->count(i) == 1;
                if (shouldLoad)
                {
                    std::string lang = chunk.getLang();
                    auto&       map  = _node_of_name[lang];
                    for (auto pair : chunk.getPairs())
                    {
                        try
                        {
                            std::string_view sv = _string_pool.intern(pair.getKey());
                            map[sv]             = pair.getValue();
                        }
                        catch (...)
                        {
                            std::string_view sv = _string_pool.intern("?");
                            map[sv]             = pair.getValue();
                            io::OutputStream(_output, io::OutputChannel::Error, true) << "Error converting UTF-8 to string for node_of_name value " << pair.getValue();
                        }
                    }
                }
#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded node_of_name chunk " << i + 1 << "/" << nodeOfNameChunkCount;
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif
            }
#ifdef NDEBUG
            io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
#endif
        }

        struct ManifestChunkRef
        {
            uint32_t    chunk_index = 0;
            uint64_t    source_offset = 0;
            uint64_t    length = 0;
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
            bool            is_v2 = false;
            bool            is_v3 = false;
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

        static size_t skip_json_ws(std::string_view s, size_t pos)
        {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
            {
                ++pos;
            }
            return pos;
        }

        static size_t find_json_key_position(std::string_view text, const std::string& key)
        {
            const std::string marker = '"' + key + '"';
            size_t search_pos = 0;
            while (search_pos < text.size())
            {
                const size_t key_pos = text.find(marker, search_pos);
                if (key_pos == std::string_view::npos)
                {
                    return std::string_view::npos;
                }

                bool in_string = false;
                bool escaped  = false;
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

        static std::string_view find_json_object(std::string_view text, const std::string& key)
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

        static bool parse_json_number_field(std::string_view object_json, const std::string& key, uint64_t& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos = object_json.find(':', pos + key_marker.size());
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

        static bool parse_json_string_field(std::string_view object_json, const std::string& key, std::string& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos = object_json.find(':', pos + key_marker.size());
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
                        case '"': result.push_back('"'); break;
                        case '\\': result.push_back('\\'); break;
                        case '/': result.push_back('/'); break;
                        case 'b': result.push_back('\b'); break;
                        case 'f': result.push_back('\f'); break;
                        case 'n': result.push_back('\n'); break;
                        case 'r': result.push_back('\r'); break;
                        case 't': result.push_back('\t'); break;
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

        static bool parse_json_bool_field(std::string_view object_json, const std::string& key, bool& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos = object_json.find(':', pos + key_marker.size());
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

        static bool parse_json_number_array_field(std::string_view object_json, const std::string& key, std::vector<uint64_t>& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size() || object_json[pos] != '[')
            {
                return false;
            }

            size_t array_end = 0;
            auto   array_json = extract_balanced(object_json, pos, '[', ']', array_end);
            if (array_json.empty())
            {
                return false;
            }

            out.clear();
            size_t cursor = pos + 1;
            const size_t stop = pos + array_json.size() - 1;
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

        static bool parse_json_string_array_field(std::string_view object_json, const std::string& key, std::vector<std::string>& out)
        {
            size_t pos = find_json_key_position(object_json, key);
            if (pos == std::string_view::npos)
            {
                return false;
            }

            const std::string key_marker = '"' + key + '"';
            pos = object_json.find(':', pos + key_marker.size());
            if (pos == std::string_view::npos)
            {
                return false;
            }

            pos = skip_json_ws(object_json, pos + 1);
            if (pos >= object_json.size() || object_json[pos] != '[')
            {
                return false;
            }

            size_t array_end = 0;
            auto   array_json = extract_balanced(object_json, pos, '[', ']', array_end);
            if (array_json.empty())
            {
                return false;
            }

            out.clear();
            size_t cursor = pos + 1;
            const size_t stop = pos + array_json.size() - 1;
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
                bool escaped = false;
                while (cursor < stop)
                {
                    char c = object_json[cursor++];
                    if (escaped)
                    {
                        switch (c)
                        {
                            case '"': value.push_back('"'); break;
                            case '\\': value.push_back('\\'); break;
                            case '/': value.push_back('/'); break;
                            case 'b': value.push_back('\b'); break;
                            case 'f': value.push_back('\f'); break;
                            case 'n': value.push_back('\n'); break;
                            case 'r': value.push_back('\r'); break;
                            case 't': value.push_back('\t'); break;
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

        static size_t parse_balanced_span(std::string_view text, size_t start_pos, char open, char close)
        {
            if (start_pos >= text.size() || text[start_pos] != open)
            {
                return std::string_view::npos;
            }

            int  depth = 0;
            bool in_string = false;
            bool escaped = false;
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

        static std::string_view extract_balanced(std::string_view text, size_t start_pos, char open, char close, size_t& next_pos)
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

        static ManifestSection parse_section_chunks(std::string_view section_object, const std::string& section_name)
        {
            const auto section_obj_view = find_json_object(section_object, section_name);
            if (section_obj_view.empty())
            {
                throw std::runtime_error("Manifest section missing: " + section_name);
            }

            const std::string chunks_key = "\"chunks\"";
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

            size_t array_end = 0;
            auto   section_array = extract_balanced(section_obj_view, array_start, '[', ']', array_end);
            if (section_array.empty())
            {
                throw std::runtime_error("Manifest section malformed (unclosed '['): " + section_name);
            }

            ManifestSection section;
            size_t         cursor = array_start + 1;
            const size_t   array_stop = array_start + section_array.size() - 1;
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

                size_t       obj_end = 0;
                auto         object_json = extract_balanced(section_obj_view, cursor, '{', '}', obj_end);
                if (object_json.empty())
                {
                    throw std::runtime_error("Manifest section malformed (chunk object parse): " + section_name);
                }

                ManifestChunkRef chunk_ref;
                uint64_t        chunk_index = 0;

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

        static ManifestDescription parse_manifest_file(const std::string& manifest_path)
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

            auto json_view = std::string_view(json_text);
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

        static std::filesystem::path resolve_manifest_chunk_path(const std::string& manifest_path,
                                                                const std::string& object_path,
                                                                const std::string& shard_root)
        {
            namespace fs = std::filesystem;

            const fs::path manifest_dir = fs::path(manifest_path).parent_path();
            std::string     normalized = object_path;

            if (normalized.rfind("hf://", 0) == 0)
            {
                normalized = normalized.substr(5);
                if (!normalized.empty() && normalized.front() == '/')
                {
                    normalized.erase(0, 1);
                }
            }

            const fs::path local_obj_path = fs::path(normalized);
            std::vector<fs::path> candidates = {local_obj_path, manifest_dir / local_obj_path};

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
                    const fs::path maybe_tail{tail};
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

        static std::filesystem::path resolve_manifest_local_path(const std::string& manifest_path,
                                                                 const std::string& local_path,
                                                                 const std::string& shard_root)
        {
            namespace fs = std::filesystem;

            if (local_path.empty())
            {
                return {};
            }

            const fs::path direct{local_path};
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

        static std::filesystem::path resolve_node_route_index_path(const std::string&         manifest_path,
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

        static std::string_view find_json_array(std::string_view text, const std::string& key)
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

        static bool array_contains_uint64(const std::vector<uint64_t>& values, uint64_t needle)
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

        static bool array_contains_string(const std::vector<std::string>& values, const std::string& needle)
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

        static void collect_route_section_matches(std::string_view               routing_obj,
                                                  const std::string&            section_name,
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

                size_t obj_end = 0;
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

        static RouteSelectionResolution resolve_route_selection(const std::string&               manifest_path,
                                                                const ManifestDescription&       manifest,
                                                                const Zelph::BinChunkSelection&  selection,
                                                                const std::string&               shard_root)
        {
            if (!manifest.node_route_supported && manifest.node_route_index_path.empty() && manifest.node_route_index_local_path.empty())
            {
                throw std::runtime_error("Manifest does not advertise nodeRouteIndex support");
            }

            const auto route_path = resolve_node_route_index_path(manifest_path, manifest, shard_root);
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
                collect_route_section_matches(routing_obj, "left", [&](std::string_view object_json) {
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
                    }
                });

                collect_route_section_matches(routing_obj, "right", [&](std::string_view object_json) {
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
                    }
                });

                collect_route_section_matches(routing_obj, "nameOfNode", [&](std::string_view object_json) {
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
                    }
                });
            }

            if (selection.route_name_explicit)
            {
                collect_route_section_matches(routing_obj, "nodeOfName", [&](std::string_view object_json) {
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
                    }
                });
            }

            if (!resolved.any_match)
            {
                throw std::runtime_error("nodeRouteIndex resolved no matching chunks for requested route selectors");
            }

            return resolved;
        }

        static FILE* open_file_or_throw(const std::string& path)
        {
            FILE* file = fopen(path.c_str(), "rb");
            if (!file)
            {
                throw std::runtime_error("Failed to open file for reading: " + path);
            }
            return file;
        }

        static std::filesystem::path ensure_cache_dir()
        {
            namespace fs = std::filesystem;
            fs::path cache_root = fs::temp_directory_path() / "zelph-hf-cache";
            if (!fs::exists(cache_root))
            {
                fs::create_directories(cache_root);
            }
            return cache_root;
        }

        static std::string sanitize_cache_token(const std::string& in)
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

        static std::filesystem::path fetch_chunk_to_cache(const std::string& source,
                                                         const uint64_t      offset,
                                                         const uint64_t      length,
                                                         const std::string&  section_hint)
        {
            namespace fs = std::filesystem;

            std::string token = sanitize_cache_token(section_hint);
            std::string src_token = sanitize_cache_token(source);
            fs::path cache_file = ensure_cache_dir() / fs::path(token + "_" + src_token + "_" + std::to_string(offset) + "_" + std::to_string(length) + ".capnp-packed");

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

        static void seek_offset_or_throw(FILE* file, uint64_t offset)
        {
            if (offset == 0) return;
            if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
            {
                throw std::runtime_error("Failed to seek to chunk source offset");
            }
        }

        void loadLeftRightChunkFromPath(const std::string&   source_path,
                                       uint64_t              source_offset,
                                       const chunk_selector*  selection,
                                       const char*           which_name,
                                       uint32_t              section_count)
        {
            FILE* file = open_file_or_throw(source_path);
            try
            {
                seek_offset_or_throw(file, source_offset);

                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 32;
                options.nestingLimit          = 128;

                kj::FdInputStream              raw_input(fileno(file));
                kj::BufferedInputStreamWrapper buffered_input(raw_input);
                ::capnp::PackedMessageReader    chunk_message(buffered_input, options);
                auto                           chunk = chunk_message.getRoot<AdjChunk>();

                if (chunk.getWhich() != which_name)
                {
                    throw std::runtime_error("Expected chunk type " + std::string(which_name)
                                              + " but found " + chunk.getWhich().cStr());
                }

                const uint32_t chunk_index = chunk.getChunkIndex();
                const bool     should_load = (selection == nullptr) || selection->count(chunk_index) == 1;

                if (should_load)
                {
                    for (auto pair : chunk.getPairs())
                    {
                        adjacency_set adj;
                        for (auto n : pair.getAdj())
                        {
                            adj.insert(n);
                        }

                        if (std::string_view(which_name) == "left")
                        {
                            _left[pair.getNode()] = std::move(adj);
                        }
                        else
                        {
                            _right[pair.getNode()] = std::move(adj);
                        }
                    }
                }

#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Loaded " << which_name << " chunk " << chunk_index + 1 << "/" << section_count
                    << ", current size=" << (std::string_view(which_name) == "left" ? _left.size() : _right.size());
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif

                fclose(file);
            }
            catch (...)
            {
                fclose(file);
                throw;
            }
        }

        void loadNameOfNodeChunkFromPath(const std::string&   source_path,
                                        uint64_t                source_offset,
                                        const chunk_selector*    selection)
        {
            FILE* file = open_file_or_throw(source_path);
            try
            {
                seek_offset_or_throw(file, source_offset);

                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 32;
                options.nestingLimit          = 128;

                kj::FdInputStream              raw_input(fileno(file));
                kj::BufferedInputStreamWrapper buffered_input(raw_input);
                ::capnp::PackedMessageReader    chunk_message(buffered_input, options);
                auto                           chunk = chunk_message.getRoot<NameChunk>();

                const uint32_t chunk_index = chunk.getChunkIndex();
                const bool     should_load = (selection == nullptr) || selection->count(chunk_index) == 1;

                if (!should_load)
                {
                    fclose(file);
                    return;
                }

                const std::string lang = chunk.getLang();
                auto&              map  = _name_of_node[lang];
                for (auto pair : chunk.getPairs())
                {
                    try
                    {
                        std::string_view sv = _string_pool.intern(pair.getValue());
                        map[pair.getKey()] = sv;
                    }
                    catch (...)
                    {
                        std::string_view sv = _string_pool.intern("?");
                        map[pair.getKey()] = sv;
                        io::OutputStream(_output, io::OutputChannel::Error, true)
                            << "Error converting UTF-8 to string for name_of_node key " << pair.getKey();
                    }
                }

                fclose(file);
            }
            catch (...)
            {
                fclose(file);
                throw;
            }
        }

        void loadNodeOfNameChunkFromPath(const std::string&   source_path,
                                        uint64_t                source_offset,
                                        const chunk_selector*    selection)
        {
            FILE* file = open_file_or_throw(source_path);
            try
            {
                seek_offset_or_throw(file, source_offset);

                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 32;
                options.nestingLimit          = 128;

                kj::FdInputStream              raw_input(fileno(file));
                kj::BufferedInputStreamWrapper buffered_input(raw_input);
                ::capnp::PackedMessageReader    chunk_message(buffered_input, options);
                auto                           chunk = chunk_message.getRoot<NodeNameChunk>();

                const uint32_t chunk_index = chunk.getChunkIndex();
                const bool     should_load = (selection == nullptr) || selection->count(chunk_index) == 1;
                if (!should_load)
                {
                    fclose(file);
                    return;
                }

                const std::string lang = chunk.getLang();
                auto&              map  = _node_of_name[lang];
                for (auto pair : chunk.getPairs())
                {
                    try
                    {
                        std::string_view sv = _string_pool.intern(pair.getKey());
                        map[sv]             = pair.getValue();
                    }
                    catch (...)
                    {
                        std::string_view sv = _string_pool.intern("?");
                        map[sv]             = pair.getValue();
                        io::OutputStream(_output, io::OutputChannel::Error, true)
                            << "Error converting UTF-8 to string for node_of_name value " << pair.getValue();
                    }
                }

                fclose(file);
            }
            catch (...)
            {
                fclose(file);
                throw;
            }
        }

        void loadFromManifest(const std::string& manifest_path,
                             const Zelph::BinChunkSelection& selection,
                             const std::string&              shard_root,
                             const std::string&              bin_path_hint,
                             const bool                      skip_payload)
        {
            std::string local_manifest_path = manifest_path;
            if (is_hf_uri(manifest_path))
            {
                local_manifest_path = fetch_chunk_to_cache(manifest_path, 0, 0, "manifest").string();
            }

            const ManifestDescription manifest_description = parse_manifest_file(local_manifest_path);
            const std::string       header_source = bin_path_hint.empty() ? manifest_description.source_bin_path : bin_path_hint;

            if (header_source.empty())
            {
                throw std::runtime_error("Manifest requires --source-bin (or source.binPath in manifest)");
            }

            const uint32_t leftChunkCount       = static_cast<uint32_t>(manifest_description.left.chunks.size());
            const uint32_t rightChunkCount      = static_cast<uint32_t>(manifest_description.right.chunks.size());
            const uint32_t nameOfNodeChunkCount = static_cast<uint32_t>(manifest_description.name_of_node.chunks.size());
            const uint32_t nodeOfNameChunkCount = static_cast<uint32_t>(manifest_description.node_of_name.chunks.size());

            RouteSelectionResolution routed_selection;
            const bool               route_requested = selection.route_nodes_explicit || selection.route_name_explicit;
            if (route_requested)
            {
                routed_selection = resolve_route_selection(local_manifest_path, manifest_description, selection, shard_root);
            }

            const bool left_explicit = selection.left_explicit || route_requested;
            const bool right_explicit = selection.right_explicit || route_requested;
            const bool name_of_node_explicit = selection.name_of_node_explicit || route_requested;
            const bool node_of_name_explicit = selection.node_of_name_explicit || route_requested;

            auto leftSelection       = normalize_chunk_selector(selection.left, leftChunkCount, left_explicit);
            auto rightSelection      = normalize_chunk_selector(selection.right, rightChunkCount, right_explicit);
            auto nameOfNodeSelection = normalize_chunk_selector(selection.nameOfNode, nameOfNodeChunkCount, name_of_node_explicit);
            auto nodeOfNameSelection = normalize_chunk_selector(selection.nodeOfName, nodeOfNameChunkCount, node_of_name_explicit);

            leftSelection.insert(routed_selection.left.begin(), routed_selection.left.end());
            rightSelection.insert(routed_selection.right.begin(), routed_selection.right.end());
            nameOfNodeSelection.insert(routed_selection.name_of_node.begin(), routed_selection.name_of_node.end());
            nodeOfNameSelection.insert(routed_selection.node_of_name.begin(), routed_selection.node_of_name.end());

            const chunk_selector* leftSelectionPtr = left_explicit ? &leftSelection : nullptr;
            const chunk_selector* rightSelectionPtr = right_explicit ? &rightSelection : nullptr;
            const chunk_selector* nameOfNodeSelectionPtr = name_of_node_explicit ? &nameOfNodeSelection : nullptr;
            const chunk_selector* nodeOfNameSelectionPtr = node_of_name_explicit ? &nodeOfNameSelection : nullptr;

            const size_t requestedLeftChunks = left_explicit ? leftSelection.size() : leftChunkCount;
            const size_t requestedRightChunks = right_explicit ? rightSelection.size() : rightChunkCount;
            const size_t requestedNameOfNodeChunks = name_of_node_explicit ? nameOfNodeSelection.size() : nameOfNodeChunkCount;
            const size_t requestedNodeOfNameChunks = node_of_name_explicit ? nodeOfNameSelection.size() : nodeOfNameChunkCount;

            validate_chunk_selector(leftSelection, leftChunkCount, "left");
            validate_chunk_selector(rightSelection, rightChunkCount, "right");
            validate_chunk_selector(nameOfNodeSelection,
                                   nameOfNodeChunkCount,
                                   "nameOfNode");
            validate_chunk_selector(nodeOfNameSelection,
                                   nodeOfNameChunkCount,
                                   "nodeOfName");

            clear_loaded_state();

            const bool header_is_remote = is_hf_uri(header_source);
            std::string header_source_path = header_source;
            if (header_is_remote)
            {
                if (manifest_description.source_header_length_bytes == 0)
                {
                    throw std::runtime_error("Manifest headerLengthBytes required for remote source-bin loading");
                }
                header_source_path =
                    fetch_chunk_to_cache(header_source, 0, manifest_description.source_header_length_bytes, "header").string();
            }

            FILE* file = open_file_or_throw(header_source_path);
            try
            {
                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 32;
                options.nestingLimit          = 128;

                kj::FdInputStream              raw_input(fileno(file));
                kj::BufferedInputStreamWrapper buffered_input(raw_input);
                ::capnp::PackedMessageReader   main_message(buffered_input, options);
                auto                          impl = main_message.getRoot<ZelphImpl>();
                loadSmallData(impl);
                fclose(file);
            }
            catch (...)
            {
                fclose(file);
                throw;
            }

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                << "Partial loading from manifest: left chunks=" << requestedLeftChunks << "/"
                << leftChunkCount << ", right chunks=" << requestedRightChunks << "/"
                << rightChunkCount << ", nameOfNode chunks=" << requestedNameOfNodeChunks << "/"
                << nameOfNodeChunkCount << ", nodeOfName chunks=" << requestedNodeOfNameChunks << "/"
                << nodeOfNameChunkCount << ", route_requested=" << (route_requested ? "true" : "false")
                << ", skip_payload=" << (skip_payload ? "true" : "false");

            if (skip_payload)
            {
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Header-only manifest load complete.";
                return;
            }

            if (leftSelectionPtr == nullptr || !leftSelection.empty())
            {
                for (const auto& ref : manifest_description.left.chunks)
                {
                    if (leftSelection.count(ref.chunk_index) != 1) continue;

                    const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
                    const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
                    const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file =
                                        fetch_chunk_to_cache(ref.object_path, 0, source_length, "left-" + std::to_string(ref.chunk_index)).string();
                                }
                            }
                            catch (...)
                            {
                                source_file =
                                    fetch_chunk_to_cache(ref.object_path, 0, source_length, "left-" + std::to_string(ref.chunk_index)).string();
                            }
                        }
                        else
                        {
                            source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            fetch_chunk_to_cache(header_source, source_offset, source_length, "left-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }

                    try
                    {
                        loadLeftRightChunkFromPath(source_file, read_chunk_start, leftSelectionPtr, "left", manifest_description.left.chunks.size());
                    }
                    catch (const std::exception& ex)
                    {
                        if (!ref.has_source_offset)
                        {
                            throw;
                        }
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Shard chunk left/" << ref.chunk_index << " failed (" << ex.what()
                            << "); falling back to sequential bin load";
                        loadFromFile(header_source_path, selection, skip_payload);
                        return;
                    }
                }
            }

            if (rightSelectionPtr == nullptr || !rightSelection.empty())
            {
                for (const auto& ref : manifest_description.right.chunks)
                {
                    if (rightSelection.count(ref.chunk_index) != 1) continue;

                    const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
                    const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
                    const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file = fetch_chunk_to_cache(ref.object_path,
                                                                       0,
                                                                       source_length,
                                                                       "right-" + std::to_string(ref.chunk_index))
                                                      .string();
                                }
                            }
                            catch (...)
                            {
                                source_file = fetch_chunk_to_cache(ref.object_path,
                                                                   0,
                                                                   source_length,
                                                                   "right-" + std::to_string(ref.chunk_index))
                                                      .string();
                            }
                        }
                        else
                        {
                            source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            fetch_chunk_to_cache(header_source, source_offset, source_length, "right-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }

                    try
                    {
                        loadLeftRightChunkFromPath(source_file,
                                                  read_chunk_start,
                                                  rightSelectionPtr,
                                                  "right",
                                                  manifest_description.right.chunks.size());
                    }
                    catch (const std::exception& ex)
                    {
                        if (!ref.has_source_offset)
                        {
                            throw;
                        }
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Shard chunk right/" << ref.chunk_index << " failed (" << ex.what()
                            << "); falling back to sequential bin load";
                        loadFromFile(header_source_path, selection, skip_payload);
                        return;
                    }
                }
            }

            if (nameOfNodeSelectionPtr == nullptr || !nameOfNodeSelection.empty())
            {
                for (const auto& ref : manifest_description.name_of_node.chunks)
                {
                    if (nameOfNodeSelection.count(ref.chunk_index) != 1) continue;

                    const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
                    const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
                    const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file = fetch_chunk_to_cache(ref.object_path,
                                                                       0,
                                                                       source_length,
                                                                       "nameOfNode-" + std::to_string(ref.chunk_index))
                                                      .string();
                                }
                            }
                            catch (...)
                            {
                                source_file =
                                    fetch_chunk_to_cache(ref.object_path,
                                                        0,
                                                        source_length,
                                                        "nameOfNode-" + std::to_string(ref.chunk_index))
                                        .string();
                            }
                        }
                        else
                        {
                            source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            fetch_chunk_to_cache(header_source, source_offset, source_length, "nameOfNode-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }
                    try
                    {
                        loadNameOfNodeChunkFromPath(source_file, read_chunk_start, nameOfNodeSelectionPtr);
                    }
                    catch (const std::exception& ex)
                    {
                        if (!ref.has_source_offset)
                        {
                            throw;
                        }
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Shard chunk nameOfNode/" << ref.chunk_index << " failed (" << ex.what()
                            << "); falling back to sequential bin load";
                        loadFromFile(header_source_path, selection, skip_payload);
                        return;
                    }
                }
            }

            if (nodeOfNameSelectionPtr == nullptr || !nodeOfNameSelection.empty())
            {
                for (const auto& ref : manifest_description.node_of_name.chunks)
                {
                    if (nodeOfNameSelection.count(ref.chunk_index) != 1) continue;

                    const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
                    const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
                    const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file = fetch_chunk_to_cache(ref.object_path,
                                                                       0,
                                                                       source_length,
                                                                       "nodeOfName-" + std::to_string(ref.chunk_index))
                                                      .string();
                                }
                            }
                            catch (...)
                            {
                                source_file =
                                    fetch_chunk_to_cache(ref.object_path,
                                                        0,
                                                        source_length,
                                                        "nodeOfName-" + std::to_string(ref.chunk_index))
                                        .string();
                            }
                        }
                        else
                        {
                            source_file = resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            fetch_chunk_to_cache(header_source, source_offset, source_length, "nodeOfName-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }
                    try
                    {
                        loadNodeOfNameChunkFromPath(source_file, read_chunk_start, nodeOfNameSelectionPtr);
                    }
                    catch (const std::exception& ex)
                    {
                        if (!ref.has_source_offset)
                        {
                            throw;
                        }
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Shard chunk nodeOfName/" << ref.chunk_index << " failed (" << ex.what()
                            << "); falling back to sequential bin load";
                        loadFromFile(header_source_path, selection, skip_payload);
                        return;
                    }
                }
            }

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after partial load: " << _string_pool.size();
        }
        void saveToFile(const std::string& filename) const
        {
            const size_t chunkSize = 1000000; // 1M entries per chunk

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: probabilities size=" << _probabilities.size() << ", left size=" << _left.size() << ", right size=" << _right.size();
            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: name_of_node outer size=" << _name_of_node.size() << ", node_of_name outer size=" << _node_of_name.size();
            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: string pool size=" << _string_pool.size();

#ifdef _WIN32
    #define fileno _fileno
#endif
            FILE* file = fopen(filename.c_str(), "wb");
            if (!file)
            {
                throw std::runtime_error("Failed to open file for writing: " + filename);
            }
            auto               fileGuard = std::unique_ptr<FILE, decltype(&fclose)>(file, &fclose);
            kj::FdOutputStream output(fileno(file));

            // Main message (small data)
            ::capnp::MallocMessageBuilder mainMessage(1u << 26);
            auto                          impl = mainMessage.initRoot<ZelphImpl>();

            // Serialize probabilities
            auto   probs = impl.initProbabilities(_probabilities.size());
            size_t idx   = 0;
            for (const auto& p : _probabilities)
            {
                probs[idx].setHash(p.first);
                probs[idx].setProb(static_cast<double>(p.second));
                ++idx;
            }
            impl.setLast(_last);
            impl.setLastVar(_last_var);

            size_t nameOfNodeChunkTotal = 0;
            for (const auto& langMap : _name_of_node)
            {
                size_t mapSize = langMap.second.size();
                nameOfNodeChunkTotal += (mapSize + chunkSize - 1) / chunkSize;
            }
            impl.setNameOfNodeChunkCount(static_cast<uint32_t>(nameOfNodeChunkTotal));

            size_t nodeOfNameChunkTotal = 0;
            for (const auto& langMap : _node_of_name)
            {
                size_t mapSize = langMap.second.size();
                nodeOfNameChunkTotal += (mapSize + chunkSize - 1) / chunkSize;
            }
            impl.setNodeOfNameChunkCount(static_cast<uint32_t>(nodeOfNameChunkTotal));

            size_t leftChunkCount  = (_left.size() + chunkSize - 1) / chunkSize;
            size_t rightChunkCount = (_right.size() + chunkSize - 1) / chunkSize;
            impl.setLeftChunkCount(static_cast<uint32_t>(leftChunkCount));
            impl.setRightChunkCount(static_cast<uint32_t>(rightChunkCount));

            ::capnp::writePackedMessage(output, mainMessage);

            // Chunk _left
            auto leftIt = _left.begin();
            for (size_t chunkIdx = 0; chunkIdx < leftChunkCount; ++chunkIdx)
            {
                ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                auto                          chunk = chunkMessage.initRoot<AdjChunk>();
                chunk.setWhich("left");
                chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

                size_t thisChunkSize = std::min(chunkSize, _left.size() - chunkIdx * chunkSize);
                auto   pairList      = chunk.initPairs(thisChunkSize);
                size_t pIdx          = 0;
                for (size_t i = 0; i < thisChunkSize; ++i, ++leftIt)
                {
                    pairList[pIdx].setNode(leftIt->first);
                    std::vector<Node> sorted(leftIt->second.begin(), leftIt->second.end());
                    std::sort(sorted.begin(), sorted.end());
                    auto adj = pairList[pIdx].initAdj(sorted.size());
                    for (size_t j = 0; j < sorted.size(); ++j)
                    {
                        adj.set(j, sorted[j]);
                    }
                    ++pIdx;
                }
                ::capnp::writePackedMessage(output, chunkMessage);
            }

            // Chunk _right
            auto rightIt = _right.begin();
            for (size_t chunkIdx = 0; chunkIdx < rightChunkCount; ++chunkIdx)
            {
                ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                auto                          chunk = chunkMessage.initRoot<AdjChunk>();
                chunk.setWhich("right");
                chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

                size_t thisChunkSize = std::min(chunkSize, _right.size() - chunkIdx * chunkSize);
                auto   pairList      = chunk.initPairs(thisChunkSize);
                size_t pIdx          = 0;
                for (size_t i = 0; i < thisChunkSize; ++i, ++rightIt)
                {
                    pairList[pIdx].setNode(rightIt->first);
                    std::vector<Node> sorted(rightIt->second.begin(), rightIt->second.end());
                    std::sort(sorted.begin(), sorted.end());
                    auto adj = pairList[pIdx].initAdj(sorted.size());
                    for (size_t j = 0; j < sorted.size(); ++j)
                    {
                        adj.set(j, sorted[j]);
                    }
                    ++pIdx;
                }
                ::capnp::writePackedMessage(output, chunkMessage);
            }

            // Chunk _name_of_node
            // Note: string_view::data() is null-terminated here because the pool
            // stores std::string objects and string_view points into them.
            for (const auto& langMap : _name_of_node)
            {
                std::string                                    lang = langMap.first;
                const auto&                                    map  = langMap.second;
                std::vector<std::pair<Node, std::string_view>> sorted(map.begin(), map.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                          { return a.first < b.first; });

                auto   it        = sorted.begin();
                size_t numChunks = (sorted.size() + chunkSize - 1) / chunkSize;
                for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx)
                {
                    ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                    auto                          chunk = chunkMessage.initRoot<NameChunk>();
                    chunk.setLang(lang);
                    chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

                    size_t thisSize = std::min(chunkSize, sorted.size() - chunkIdx * chunkSize);
                    auto   pairs    = chunk.initPairs(thisSize);
                    for (size_t i = 0; i < thisSize; ++i, ++it)
                    {
                        pairs[i].setKey(it->first);
                        // Pool-backed string_view: data() is null-terminated
                        pairs[i].setValue(it->second.data());
                    }
                    ::capnp::writePackedMessage(output, chunkMessage);
                }
            }

            // Chunk _node_of_name
            for (const auto& langMap : _node_of_name)
            {
                std::string                                    lang = langMap.first;
                const auto&                                    map  = langMap.second;
                std::vector<std::pair<std::string_view, Node>> sorted(map.begin(), map.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                          { return a.first < b.first; });

                auto   it        = sorted.begin();
                size_t numChunks = (sorted.size() + chunkSize - 1) / chunkSize;
                for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx)
                {
                    ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                    auto                          chunk = chunkMessage.initRoot<NodeNameChunk>();
                    chunk.setLang(lang);
                    chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

                    size_t thisSize = std::min(chunkSize, sorted.size() - chunkIdx * chunkSize);
                    auto   pairs    = chunk.initPairs(thisSize);
                    for (size_t i = 0; i < thisSize; ++i, ++it)
                    {
                        // Pool-backed string_view: data() is null-terminated
                        pairs[i].setKey(it->first.data());
                        pairs[i].setValue(it->second);
                    }
                    ::capnp::writePackedMessage(output, chunkMessage);
                }
            }
        }

        void loadFromFile(const std::string& filename)
        {
#ifdef _WIN32
    #define fileno _fileno
#endif
            FILE* file = fopen(filename.c_str(), "rb");
            if (!file)
            {
                throw std::runtime_error("Failed to open file for reading: " + filename);
            }

            ::capnp::ReaderOptions options;
            options.traversalLimitInWords = 1ULL << 32;
            options.nestingLimit          = 128;

            kj::FdInputStream              rawInput(fileno(file));
            kj::BufferedInputStreamWrapper bufferedInput(rawInput);

            ::capnp::PackedMessageReader mainMessage(bufferedInput, options);
            auto                         impl = mainMessage.getRoot<ZelphImpl>();

            loadSmallData(impl);

            uint32_t leftChunkCount       = impl.getLeftChunkCount();
            uint32_t rightChunkCount      = impl.getRightChunkCount();
            uint32_t nameOfNodeChunkCount = impl.getNameOfNodeChunkCount();
            uint32_t nodeOfNameChunkCount = impl.getNodeOfNameChunkCount();
            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loading: left chunks=" << leftChunkCount << ", right chunks=" << rightChunkCount
                                                                           << ", nameOfNode chunks=" << nameOfNodeChunkCount << ", nodeOfName chunks=" << nodeOfNameChunkCount;

            loadLeftRightChunks(bufferedInput, options, leftChunkCount, rightChunkCount);
            loadNameOfNodeChunks(bufferedInput, options, nameOfNodeChunkCount);
            loadNodeOfNameChunks(bufferedInput, options, nodeOfNameChunkCount);

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after load: " << _string_pool.size();

            fclose(file);
        }

        void loadFromFile(const std::string&              filename,
                          const Zelph::BinChunkSelection& selection,
                          const bool                     skip_payload)
        {
#ifdef _WIN32
    #define fileno _fileno
#endif
            FILE* file = fopen(filename.c_str(), "rb");
            if (!file)
            {
                throw std::runtime_error("Failed to open file for reading: " + filename);
            }

            try
            {
                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 32;
                options.nestingLimit          = 128;

                kj::FdInputStream              rawInput(fileno(file));
                kj::BufferedInputStreamWrapper bufferedInput(rawInput);

                ::capnp::PackedMessageReader mainMessage(bufferedInput, options);
                auto                         impl = mainMessage.getRoot<ZelphImpl>();

                clear_loaded_state();
                loadSmallData(impl);

                uint32_t leftChunkCount       = impl.getLeftChunkCount();
                uint32_t rightChunkCount      = impl.getRightChunkCount();
                uint32_t nameOfNodeChunkCount = impl.getNameOfNodeChunkCount();
                uint32_t nodeOfNameChunkCount = impl.getNodeOfNameChunkCount();
                auto leftSelector       = normalize_chunk_selector(selection.left, leftChunkCount, selection.left_explicit);
                auto rightSelector      = normalize_chunk_selector(selection.right, rightChunkCount, selection.right_explicit);
                auto nameOfNodeSelector = normalize_chunk_selector(selection.nameOfNode, nameOfNodeChunkCount, selection.name_of_node_explicit);
                auto nodeOfNameSelector = normalize_chunk_selector(selection.nodeOfName, nodeOfNameChunkCount, selection.node_of_name_explicit);

                const chunk_selector* leftSelectorPtr = selection.left_explicit ? &leftSelector : nullptr;
                const chunk_selector* rightSelectorPtr = selection.right_explicit ? &rightSelector : nullptr;
                const chunk_selector* nameOfNodeSelectorPtr = selection.name_of_node_explicit ? &nameOfNodeSelector : nullptr;
                const chunk_selector* nodeOfNameSelectorPtr = selection.node_of_name_explicit ? &nodeOfNameSelector : nullptr;

                const size_t requestedLeftChunks = selection.left_explicit ? leftSelector.size() : leftChunkCount;
                const size_t requestedRightChunks = selection.right_explicit ? rightSelector.size() : rightChunkCount;
                const size_t requestedNameOfNodeChunks = selection.name_of_node_explicit ? nameOfNodeSelector.size() : nameOfNodeChunkCount;
                const size_t requestedNodeOfNameChunks = selection.node_of_name_explicit ? nodeOfNameSelector.size() : nodeOfNameChunkCount;

                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Partial loading: left chunks=" << requestedLeftChunks << "/" << leftChunkCount
                    << ", right chunks=" << requestedRightChunks << "/" << rightChunkCount
                    << ", nameOfNode chunks=" << requestedNameOfNodeChunks << "/"
                    << nameOfNodeChunkCount
                    << ", nodeOfName chunks=" << requestedNodeOfNameChunks << "/"
                    << nodeOfNameChunkCount
                    << ", skip_payload=" << (skip_payload ? "true" : "false");

                validate_chunk_selector(leftSelector, leftChunkCount, "left");
                validate_chunk_selector(rightSelector, rightChunkCount, "right");
                validate_chunk_selector(nameOfNodeSelector, nameOfNodeChunkCount, "nameOfNode");
                validate_chunk_selector(nodeOfNameSelector, nodeOfNameChunkCount, "nodeOfName");

                if (skip_payload)
                {
                    io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Header-only file load complete.";
                    fclose(file);
                    return;
                }

                if (leftSelectorPtr == nullptr || !leftSelector.empty() || rightSelectorPtr == nullptr || !rightSelector.empty())
                {
                    loadLeftRightChunks(bufferedInput,
                                        options,
                                        leftChunkCount,
                                        rightChunkCount,
                                        leftSelectorPtr,
                                        rightSelectorPtr);
                }
                if (nameOfNodeSelectorPtr == nullptr || !nameOfNodeSelector.empty())
                {
                    loadNameOfNodeChunks(bufferedInput, options, nameOfNodeChunkCount, nameOfNodeSelectorPtr);
                }
                if (nodeOfNameSelectorPtr == nullptr || !nodeOfNameSelector.empty())
                {
                    loadNodeOfNameChunks(bufferedInput, options, nodeOfNameChunkCount, nodeOfNameSelectorPtr);
                }

                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after partial load: " << _string_pool.size();

                fclose(file);
            }
            catch (...)
            {
                fclose(file);
                throw;
            }
        }

        void transfer_names_locked(const Node from, const Node into)
        {
            if (from == into) return;

            // PRECONDITION:
            // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively,
            // always in this order: _mtx_node_of_name -> _mtx_name_of_node

            for (auto& [lang, forward] : _name_of_node)
            {
                auto it_from = forward.find(from);
                if (it_from == forward.end())
                {
                    continue;
                }

                const std::string_view from_name = it_from->second;

                auto [reverse_outer_it, inserted_reverse_outer] = _node_of_name.try_emplace(lang);
                (void)inserted_reverse_outer;
                auto& reverse = reverse_outer_it->second;

                // Remove reverse entry for the disappearing pair (from_name -> from), if it exists.
                auto rev_from_it = reverse.find(from_name);
                if (rev_from_it != reverse.end() && rev_from_it->second == from)
                {
                    reverse.erase(rev_from_it);
                }

                auto it_into = forward.find(into);
                if (it_into != forward.end())
                {
                    const std::string_view into_name = it_into->second;

                    if (into_name != from_name)
                    {
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Warning: Name conflict in language '" << lang << "': '"
                            << from_name << "' (from merged node) vs '" << into_name
                            << "'. Keeping existing name '" << into_name << "'.";
                    }

                    // Drop the old forward mapping for "from".
                    forward.erase(it_from);

                    // Repair reverse mapping for the kept name.
                    auto rev_into_it = reverse.find(into_name);
                    if (rev_into_it == reverse.end())
                    {
                        reverse.emplace(into_name, into);
                    }
                    else if (rev_into_it->second == from)
                    {
                        rev_into_it->second = into;
                    }
                }
                else
                {
                    // Move forward mapping from -> into
                    forward.erase(it_from);
                    auto [new_into_it, inserted_into] = forward.emplace(into, from_name);
                    if (!inserted_into)
                    {
                        new_into_it->second = from_name;
                    }

                    // Move reverse mapping from_name -> into
                    auto rev_it = reverse.find(from_name);
                    if (rev_it == reverse.end())
                    {
                        reverse.emplace(from_name, into);
                    }
                    else if (rev_it->second == from)
                    {
                        rev_it->second = into;
                    }
                    else if (rev_it->second != into)
                    {
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Warning: Skipping reverse mapping update for name '" << from_name
                            << "' in language '" << lang
                            << "' due to existing conflicting mapping.";
                    }
                }
            }
        }

        void transfer_names(const Node from, const Node into)
        {
            if (from == into) return;

            // Global lock order must be consistent everywhere:
            // _mtx_node_of_name -> _mtx_name_of_node
            std::unique_lock lock_node(_mtx_node_of_name);
            std::unique_lock lock_name(_mtx_name_of_node);

            transfer_names_locked(from, into);
        }

        void assign_name_locked(const Node node, const std::string& name, const std::string& lang)
        {
            // PRECONDITION:
            // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
            // in this order: _mtx_node_of_name -> _mtx_name_of_node

            auto [rev_outer_it, rev_outer_inserted] = _node_of_name.try_emplace(lang);
            auto [fwd_outer_it, fwd_outer_inserted] = _name_of_node.try_emplace(lang);
            (void)rev_outer_inserted;
            (void)fwd_outer_inserted;

            auto& rev = rev_outer_it->second; // name -> node
            auto& fwd = fwd_outer_it->second; // node -> name

            // 1. Remove old name of this node, if different
            auto fwd_it = fwd.find(node);
            if (fwd_it != fwd.end() && fwd_it->second != name)
            {
                auto old_rev_it = rev.find(fwd_it->second);
                if (old_rev_it != rev.end() && old_rev_it->second == node)
                {
                    rev.erase(old_rev_it);
                }
                fwd.erase(fwd_it);
            }

            // 2. Intern new name
            std::string_view sv = _string_pool.intern(name);

            // 3. If another node currently owns that name, detach its forward mapping
            auto rev_it = rev.find(sv);
            if (rev_it != rev.end() && rev_it->second != node)
            {
                const Node previous_owner = rev_it->second;

                auto prev_fwd_it = fwd.find(previous_owner);
                if (prev_fwd_it != fwd.end() && prev_fwd_it->second == sv)
                {
                    fwd.erase(prev_fwd_it);
                }

                rev_it->second = node;
            }
            else if (rev_it == rev.end())
            {
                rev.emplace(sv, node);
            }

            // 4. Write/repair forward mapping
            auto [new_fwd_it, inserted] = fwd.emplace(node, sv);
            if (!inserted)
            {
                new_fwd_it->second = sv;
            }
        }

        void remove_name_locked(const Node node, const std::string& lang)
        {
            // PRECONDITION:
            // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
            // in this order: _mtx_node_of_name -> _mtx_name_of_node

            auto fwd_outer_it = _name_of_node.find(lang);
            if (fwd_outer_it == _name_of_node.end())
            {
                return;
            }

            auto& fwd    = fwd_outer_it->second;
            auto  fwd_it = fwd.find(node);
            if (fwd_it == fwd.end())
            {
                return;
            }

            const std::string_view old_name = fwd_it->second;
            fwd.erase(fwd_it);

            auto rev_outer_it = _node_of_name.find(lang);
            if (rev_outer_it == _node_of_name.end())
            {
                return;
            }

            auto& rev    = rev_outer_it->second;
            auto  rev_it = rev.find(old_name);
            if (rev_it != rev.end() && rev_it->second == node)
            {
                rev.erase(rev_it);
            }
        }

        size_t cleanup_dangling_names()
        {
            size_t removed_count = 0;

            ankerl::unordered_dense::set<Node> valid_nodes;
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                valid_nodes.reserve(_left.size());
                for (const auto& pair : _left)
                {
                    valid_nodes.insert(pair.first);
                }
            }

            {
                std::unique_lock lock(_mtx_name_of_node);
                for (auto& lang_pair : _name_of_node)
                {
                    auto& map = lang_pair.second;
                    for (auto it = map.begin(); it != map.end();)
                    {
                        if (valid_nodes.count(it->first) == 0)
                        {
                            it = map.erase(it);
                            removed_count++;
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
            }

            {
                std::unique_lock lock(_mtx_node_of_name);
                for (auto& lang_pair : _node_of_name)
                {
                    auto& map = lang_pair.second;
                    for (auto it = map.begin(); it != map.end();)
                    {
                        if (valid_nodes.count(it->second) == 0)
                        {
                            it = map.erase(it);
                            removed_count++;
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
            }

            // Note: removed strings remain in _string_pool (append-only).

            return removed_count;
        }

        void remove_node_names(Node nd)
        {
            std::unique_lock lock1(_mtx_node_of_name);
            std::unique_lock lock2(_mtx_name_of_node);

            // Remove forward mappings (node → name) in all languages
            for (auto& lang_map : _name_of_node)
            {
                lang_map.second.erase(nd);
            }

            // Remove reverse mappings (name → node) for this node in all languages
            for (auto& lang_map : _node_of_name)
            {
                auto& map = lang_map.second;
                for (auto it = map.begin(); it != map.end();)
                {
                    if (it->second == nd)
                    {
                        it = map.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }

        void emit(io::OutputChannel channel, const std::string& text, bool newline = true) const
        {
            if (_output)
                _output(io::OutputEvent{channel, text, newline});
        }

        using name_of_node_map = ankerl::unordered_dense::map<Node, std::string_view>;
        using node_of_name_map = ankerl::unordered_dense::map<std::string_view, Node>;

        StringPool _string_pool;

        ankerl::unordered_dense::map<std::string, name_of_node_map> _name_of_node; // key is language identifier
        ankerl::unordered_dense::map<std::string, node_of_name_map> _node_of_name; // key is language identifier

        mutable std::shared_mutex    _mtx_node_of_name;
        mutable std::shared_mutex    _mtx_name_of_node;
        mutable std::recursive_mutex _mtx_print;

        mutable std::shared_mutex                                              _fs_cache_mtx;
        mutable ankerl::unordered_dense::map<Node, std::vector<FactStructure>> _fs_cache;
        mutable std::atomic<bool>                                              _fs_cache_has_entries{false};

        int               _max_log_depth{0};
        bool              _logging{false};
        io::OutputHandler _output;
    };
}
