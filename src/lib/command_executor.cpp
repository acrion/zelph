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

#include "command_executor.hpp"

#include "chrono/stopwatch.hpp"
#include "io/data_manager.hpp"
#include "io/mermaid.hpp"
#include "network/network.hpp"
#include "network/reasoning.hpp"
#include "platform/platform_utils.hpp"
#include "repl_state.hpp"
#include "script_engine.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "versions.hpp"

#ifndef __EMSCRIPTEN__
    #include "wikidata/wikidata.hpp"

    #include "zelph.capnp.h"

    #include <capnp/message.h>
    #include <capnp/serialize-packed.h>
    #include <kj/io.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

using namespace zelph;

// Resolve a script reference for .import and command-line scripts.
// Resolution order:
//   1. the path as given (absolute, or relative to the current working
//      directory), with the ".zph" extension being optional
//   2. the zelph standard library directories (see
//      platform::get_standard_library_paths), same extension rule
// ".zph" scripts are fed line by line through the REPL pipeline; ".janet"
// scripts are executed as whole Janet programs (see
// ScriptEngine::run_janet_script). The ".janet" extension must be spelled
// out; a bare name still resolves to ".zph". Other extensions are rejected.
static std::string resolve_script_path(const std::string& raw)
{
    namespace fs = std::filesystem;

    const std::string ext = fs::path(raw).extension().string();
    if (!ext.empty() && ext != ".zph" && ext != ".janet")
        throw std::runtime_error("Script '" + raw + "': only '.zph' and '.janet' scripts can be imported (the '.zph' extension may be omitted)");

    std::vector<fs::path> variants;
    variants.emplace_back(raw);
    if (ext.empty())
        variants.emplace_back(raw + ".zph");

    for (const auto& v : variants)
    {
        std::error_code ec;
        if (fs::is_regular_file(v, ec)) return v.string();
    }

    if (!fs::path(raw).is_absolute())
    {
        for (const auto& base : platform::get_standard_library_paths())
        {
            for (const auto& v : variants)
            {
                std::error_code ec;
                const fs::path  candidate = base / v;
                if (fs::is_regular_file(candidate, ec)) return candidate.string();
            }
        }
    }

    throw std::runtime_error("Script '" + raw + "' not found (searched the given path and the zelph standard library; see '.help .import')");
}

// Default module ID of a script: the lowercase filename stem, e.g.
// "binary-arithmetic" for /path/binary-arithmetic.zph. ASCII lowercasing
// is sufficient -- module IDs are file names under the script author's
// control.
static std::string default_module_id(const std::string& resolved_path)
{
    return string::to_lower_ascii(std::filesystem::path(resolved_path).stem().string());
}

// Collect the module IDs a .zph script registers: its default ID plus
// every ID declared by a ".provides" line. The pre-scan runs BEFORE the
// script is executed, so substitutability works in both directions: a
// script whose .provides alias is already claimed by an alternative
// implementation is skipped without loading anything. Convention:
// .provides belongs at the top of a script (the scan is line-based and
// would also pick up a ".provides" line inside a Janet block).
static std::vector<std::string> scan_module_ids(const std::string& resolved)
{
    std::vector<std::string> ids;
    ids.push_back(default_module_id(resolved));

    std::ifstream stream(resolved);
    for (std::string line; std::getline(stream, line);)
    {
        const std::vector<std::string> parts = zelph::string::tokenize_quoted(line);
        if (parts.size() >= 2 && parts[0] == ".provides")
        {
            for (size_t i = 1; i < parts.size(); ++i)
                ids.push_back(string::to_lower_ascii(parts[i]));
        }
    }
    return ids;
}

// True if the text is wrapped in ONE pair of parentheses enclosing the
// whole string -- "(a done b)" but not "(a p b) mark ok". Quoted sections
// are skipped, so a predicate like "is (not) father of" cannot mislead it.
static bool is_fully_parenthesized(const std::string& s)
{
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return false;

    int  depth    = 0;
    bool in_quote = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"')
        {
            in_quote = !in_quote;
            continue;
        }
        if (in_quote) continue;

        if (s[i] == '(')
        {
            ++depth;
        }
        else if (s[i] == ')')
        {
            if (--depth == 0) return i + 1 == s.size();
        }
    }
    return false;
}

// Alternative spellings of a command. One table drives BOTH the dispatch
// registration and ".help <alias>", so an alias can never exist as a
// runnable command while ".help" claims not to know it.
static const std::map<std::string, std::string> command_aliases = {
    {".why", ".explain"},
};

class console::CommandExecutor::Impl
{
public:
    Impl(network::Reasoning*            n,
         ScriptEngine*                  se,
         std::shared_ptr<ReplState>     rs,
         CommandExecutor::LineProcessor lp)
        : _n(n)
        , _script_engine(se)
        , _repl_state(std::move(rs))
        , _process_line_callback(std::move(lp))
    {
        register_commands();
    }

    // Everything the line reader may still be holding on to. Both callers
    // reach the same situation from different directions: a script's last
    // line, and end of input in the REPL. The keyword handler's
    // :incomplete veto does not apply any more -- there are no further
    // lines to wait for -- so dispatch is forced, which turns an
    // unterminated block into an error instead of silence.
    void finish_input() const
    {
        if (_repl_state->accumulating_keyword)
        {
            std::string keyword               = _repl_state->active_keyword;
            std::string text                  = _repl_state->keyword_buffer;
            _repl_state->accumulating_keyword = false;
            _repl_state->active_keyword.clear();
            _repl_state->keyword_buffer.clear();
            _repl_state->keyword_prev_blank = false;
            _script_engine->invoke_keyword(keyword, text, /*force*/ true);
        }

        if (_repl_state->accumulating_zelph && !_repl_state->zelph_buffer.empty())
        {
            const std::string buffered    = _repl_state->zelph_buffer;
            const std::string transformed = _script_engine->parse_zelph_to_janet(buffered);
            _repl_state->zelph_buffer.clear();
            _repl_state->accumulating_zelph = false;

            if (transformed.empty())
            {
                // Still incomplete with no more lines coming. Silence here
                // meant a truncated script or paste looked like it had run.
                throw std::runtime_error("Input ends inside an unfinished statement: " + string::trim(buffered));
            }
            _script_engine->process_janet(transformed, true);
        }
        _repl_state->accumulating_zelph = false;

        if (!_repl_state->janet_buffer.empty())
        {
            _script_engine->process_janet(_repl_state->janet_buffer, false);
            _repl_state->janet_buffer.clear();
        }
        _repl_state->accumulating_inline_janet = false;
        _repl_state->script_mode               = ScriptMode::Zelph;
    }

    void execute(const std::vector<std::string>& cmd)
    {
        if (cmd.empty()) return;

        auto it = _command_map.find(cmd[0]);
        if (it != _command_map.end())
        {
            // Execute the specific handler
            it->second(cmd);
        }
        else
        {
            throw std::runtime_error("Unknown command " + cmd[0] + ". Type .help for a list.");
        }
    }

private:
    // --- Context References ---
    network::Reasoning*              _n;
    ScriptEngine*                    _script_engine;
    std::shared_ptr<io::DataManager> _data_manager;
    std::shared_ptr<ReplState>       _repl_state;
    CommandExecutor::LineProcessor   _process_line_callback;

    // --- Dispatch Map ---
    using Handler = std::function<void(const std::vector<std::string>&)>;
    std::map<std::string, Handler> _command_map;

    // --- Registration ---
    void register_commands()
    {
        _command_map[".help"] = [this](auto& c)
        { cmd_help(c); };
        _command_map[".quit"] = [](auto& c) { /* Exit handled by caller loop, usually acts as no-op here or throws */ };
        _command_map[".lang"] = [this](auto& c)
        { cmd_lang(c); };
        _command_map[".name"] = [this](auto& c)
        { cmd_name(c); };
        _command_map[".delname"] = [this](auto& c)
        { cmd_delname(c); };
        _command_map[".node"] = [this](auto& c)
        { cmd_node(c); };
        _command_map[".list"] = [this](auto& c)
        { cmd_list(c); };
        _command_map[".clist"] = [this](auto& c)
        { cmd_clist(c); };
        _command_map[".out"] = [this](auto& c)
        { cmd_connections(c, true); };
        _command_map[".in"] = [this](auto& c)
        { cmd_connections(c, false); };
        _command_map[".remove"] = [this](auto& c)
        { cmd_remove(c); };
        _command_map[".mermaid"] = [this](auto& c)
        { cmd_mermaid(c); };
        _command_map[".run"] = [this](auto& c)
        { cmd_run(c); };
        _command_map[".run-once"] = [this](auto& c)
        { cmd_run_once(c); };
        _command_map[".run-delta"] = [this](auto& c)
        { cmd_run_delta(c); };
#ifndef __EMSCRIPTEN__
        _command_map[".run-export"] = [this](auto& c)
        { cmd_run_export(c); };
        _command_map[".load"] = [this](auto& c)
        { cmd_load(c); };
        _command_map[".load-partial"] = [this](auto& c)
        { cmd_load_partial(c); };
        _command_map[".wikidata-constraints"] = [this](auto& c)
        { cmd_wikidata_constraints(c); };
        _command_map[".wikidata-qualifiers"] = [this](auto& c)
        { cmd_wikidata_qualifiers(c); };
#endif
        _command_map[".list-rules"] = [this](auto& c)
        { cmd_list_rules(c); };
        _command_map[".list-predicate-usage"] = [this](auto& c)
        { cmd_list_predicate_usage(c); };
        _command_map[".list-predicate-value-usage"] = [this](auto& c)
        { cmd_list_predicate_value_usage(c); };
        _command_map[".remove-rules"] = [this](auto& c)
        { cmd_remove_rules(c); };
        _command_map[".prune-facts"] = [this](auto& c)
        { cmd_prune(c, true); };
        _command_map[".prune-nodes"] = [this](auto& c)
        { cmd_prune(c, false); };
        _command_map[".cleanup"] = [this](auto& c)
        { cmd_cleanup(c); };
        _command_map[".new"] = [this](auto& c)
        { cmd_new(c); };
        _command_map[".stat"] = [this](auto& c)
        { cmd_stat(c); };
#ifndef __EMSCRIPTEN__
        _command_map[".stat-file"] = [this](auto& c)
        { cmd_stat_file(c); };
        _command_map[".index-file"] = [this](auto& c)
        { cmd_index_file(c); };
#endif
        _command_map[".licenses"] = [this](auto& c)
        { cmd_licenses(c); };
        _command_map[".log"] = [this](auto& c)
        { cmd_log(c); };
        _command_map[".log-janet"] = [this](auto& c)
        { cmd_log_janet(c); };
        _command_map[".prof"] = [this](auto& c)
        { cmd_prof(c); };
#ifndef __EMSCRIPTEN__
        _command_map[".save"] = [this](auto& c)
        { cmd_save(c); };
        _command_map[".save-predicates"] = [this](auto& c)
        { cmd_save_predicates(c); };
#endif
        _command_map[".import"] = [this](auto& c)
        { cmd_import(c); };
        _command_map[".provides"] = [this](auto& c)
        { cmd_provides(c); };
        _command_map[".auto-run"] = [this](auto& c)
        { cmd_auto_run(c); };
        _command_map[".deductions"] = [this](auto& c)
        { cmd_deductions(c); };
#ifndef __EMSCRIPTEN__
        _command_map[".export-wikidata"] = [this](auto& c)
        { cmd_export_wikidata(c); };
#endif
        _command_map[".parallel"] = [this](auto& c)
        { cmd_parallel(c); };
        _command_map[".anchors"] = [this](auto& c)
        { cmd_anchors(c); };
        _command_map[".semi-naive"] = [this](auto& c)
        { cmd_semi_naive(c); };
        _command_map[".fact-stores"] = [this](auto& c)
        { cmd_fact_stores(c); };
        _command_map[".cluster"] = [this](auto& c)
        { cmd_cluster(c); };
        _command_map[".cluster-drop"] = [this](auto& c)
        { cmd_cluster_drop(c); };
        _command_map[".cluster-merge"] = [this](auto& c)
        { cmd_cluster_merge(c); };
        _command_map[".explain"] = [this](auto& c)
        { cmd_explain(c); };

        for (const auto& [alias, canonical] : command_aliases)
            _command_map[alias] = _command_map.at(canonical);
    }

// --- Helpers ---
#ifndef __EMSCRIPTEN__
    class CountingInputStream : public kj::InputStream
    {
    public:
        explicit CountingInputStream(kj::InputStream& inner)
            : _inner(inner)
        {
        }

        size_t tryRead(void* buffer, size_t minBytes, size_t maxBytes) override
        {
            auto n = _inner.tryRead(buffer, minBytes, maxBytes);
            _count += n;
            return n;
        }

        void skip(size_t bytes) override
        {
            _inner.skip(bytes);
            _count += bytes;
        }

        uint64_t bytes_read() const
        {
            return _count;
        }

    private:
        kj::InputStream& _inner;
        uint64_t         _count{0};
    };

    class CountingBufferedInputStream : public kj::BufferedInputStream
    {
    public:
        explicit CountingBufferedInputStream(kj::InputStream& inner)
            : _buffered(inner)
        {
        }

        kj::ArrayPtr<const kj::byte> tryGetReadBuffer() override
        {
            return _buffered.tryGetReadBuffer();
        }

        size_t tryRead(void* buffer, size_t minBytes, size_t maxBytes) override
        {
            auto n = _buffered.tryRead(buffer, minBytes, maxBytes);
            _count += n;
            return n;
        }

        void skip(size_t bytes) override
        {
            _buffered.skip(bytes);
            _count += bytes;
        }

        uint64_t bytes_read() const
        {
            return _count;
        }

    private:
        kj::BufferedInputStreamWrapper _buffered;
        uint64_t                       _count{0};
    };

    struct BinHeaderStats
    {
        uint32_t left_chunk_count   = 0;
        uint32_t right_chunk_count  = 0;
        uint32_t name_of_node_count = 0;
        uint32_t node_of_name_count = 0;
        uint64_t file_size_bytes    = 0;
    };

    struct BinChunkRef
    {
        uint32_t    chunk_index = 0;
        uint64_t    offset      = 0;
        uint64_t    length      = 0;
        std::string which;
        std::string lang;
    };

    struct BinIndexData
    {
        std::string              filename;
        BinHeaderStats           stats;
        uint64_t                 header_length_bytes = 0;
        std::vector<BinChunkRef> left_chunks;
        std::vector<BinChunkRef> right_chunks;
        std::vector<BinChunkRef> name_of_node_chunks;
        std::vector<BinChunkRef> node_of_name_chunks;
    };

    static ::capnp::ReaderOptions make_bin_reader_options()
    {
        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;
        return options;
    }

    static BinHeaderStats read_bin_header_stats(const std::string& filename)
    {
        FILE* file = fopen(filename.c_str(), "rb");
        if (!file)
        {
            throw std::runtime_error("Command .stat-file: Failed to open file '" + filename + "'");
        }

        try
        {
            BinHeaderStats stats;
            stats.file_size_bytes = std::filesystem::file_size(filename);

            kj::FdInputStream              raw_input(fileno(file));
            kj::BufferedInputStreamWrapper buffered_input(raw_input);

            ::capnp::ReaderOptions options;
            options.traversalLimitInWords = 1ULL << 32;
            options.nestingLimit          = 128;

            ::capnp::PackedMessageReader main_message(buffered_input, options);
            auto                         impl = main_message.getRoot<zelph::network::ZelphImpl>();

            stats.left_chunk_count   = impl.getLeftChunkCount();
            stats.right_chunk_count  = impl.getRightChunkCount();
            stats.name_of_node_count = impl.getNameOfNodeChunkCount();
            stats.node_of_name_count = impl.getNodeOfNameChunkCount();

            fclose(file);
            return stats;
        }
        catch (...)
        {
            fclose(file);
            throw;
        }
    }

    static BinIndexData read_bin_index_data(const std::string& filename)
    {
        FILE* file = fopen(filename.c_str(), "rb");
        if (!file)
        {
            throw std::runtime_error("Command .index-file: Failed to open file '" + filename + "'");
        }

        try
        {
            BinIndexData data;
            data.filename              = filename;
            data.stats.file_size_bytes = std::filesystem::file_size(filename);

            kj::FdInputStream           raw_input(fileno(file));
            CountingBufferedInputStream counting_input(raw_input);
            auto                        options = make_bin_reader_options();

            // A PackedMessageReader reads its segments lazily. Here we only touch the
            // root struct (chunkIndex / which / lang), which lives in segment 0, so
            // any further segments stay unread until the reader is destroyed. The
            // destructor then skips the remaining segments to position the stream at
            // the start of the next message. We must therefore measure the consumed
            // byte count *after* the reader has been destroyed; measuring it while
            // the reader is still alive yields a length that only covers segment 0,
            // which produces a gap before the next chunk for large multi-segment
            // chunks (those exceeding the 512 MiB first segment of saveToFile's
            // MallocMessageBuilder(1u << 26)).
            uint64_t header_offset = counting_input.bytes_read();
            {
                ::capnp::PackedMessageReader main_message(counting_input, options);
                auto                         impl = main_message.getRoot<zelph::network::ZelphImpl>();
                data.stats.left_chunk_count       = impl.getLeftChunkCount();
                data.stats.right_chunk_count      = impl.getRightChunkCount();
                data.stats.name_of_node_count     = impl.getNameOfNodeChunkCount();
                data.stats.node_of_name_count     = impl.getNodeOfNameChunkCount();
            }
            data.header_length_bytes = counting_input.bytes_read() - header_offset;

            auto read_adj_chunks = [&](uint32_t                  count,
                                       std::vector<BinChunkRef>& target)
            {
                target.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint64_t    before = counting_input.bytes_read();
                    BinChunkRef ref;
                    ref.offset = before;
                    {
                        ::capnp::PackedMessageReader chunk_message(counting_input, options);
                        auto                         chunk = chunk_message.getRoot<zelph::network::AdjChunk>();
                        ref.chunk_index                    = chunk.getChunkIndex();
                        ref.which                          = chunk.getWhich().cStr();
                    }
                    // Measured after chunk_message is destroyed: the destructor has now
                    // skipped all remaining segments of this message.
                    ref.length = counting_input.bytes_read() - before;
                    target.push_back(std::move(ref));
                }
            };

            read_adj_chunks(data.stats.left_chunk_count, data.left_chunks);
            read_adj_chunks(data.stats.right_chunk_count, data.right_chunks);

            data.name_of_node_chunks.reserve(data.stats.name_of_node_count);
            for (uint32_t i = 0; i < data.stats.name_of_node_count; ++i)
            {
                uint64_t    before = counting_input.bytes_read();
                BinChunkRef ref;
                ref.offset = before;
                {
                    ::capnp::PackedMessageReader chunk_message(counting_input, options);
                    auto                         chunk = chunk_message.getRoot<zelph::network::NameChunk>();
                    ref.chunk_index                    = chunk.getChunkIndex();
                    ref.lang                           = chunk.getLang().cStr();
                }
                ref.length = counting_input.bytes_read() - before;
                data.name_of_node_chunks.push_back(std::move(ref));
            }

            data.node_of_name_chunks.reserve(data.stats.node_of_name_count);
            for (uint32_t i = 0; i < data.stats.node_of_name_count; ++i)
            {
                uint64_t    before = counting_input.bytes_read();
                BinChunkRef ref;
                ref.offset = before;
                {
                    ::capnp::PackedMessageReader chunk_message(counting_input, options);
                    auto                         chunk = chunk_message.getRoot<zelph::network::NodeNameChunk>();
                    ref.chunk_index                    = chunk.getChunkIndex();
                    ref.lang                           = chunk.getLang().cStr();
                }
                ref.length = counting_input.bytes_read() - before;
                data.node_of_name_chunks.push_back(std::move(ref));
            }

            fclose(file);
            return data;
        }
        catch (...)
        {
            fclose(file);
            throw;
        }
    }

    static void write_bin_index_json(const BinIndexData& data, const std::string& output_filename)
    {
        std::ofstream out(output_filename);
        if (!out.is_open())
        {
            throw std::runtime_error("Command .index-file: Failed to open output file '" + output_filename + "'");
        }

        auto escape_json_string = [](const std::string& value)
        {
            std::ostringstream escaped;
            for (unsigned char ch : value)
            {
                switch (ch)
                {
                case '\"':
                    escaped << "\\\"";
                    break;
                case '\\':
                    escaped << "\\\\";
                    break;
                case '\b':
                    escaped << "\\b";
                    break;
                case '\f':
                    escaped << "\\f";
                    break;
                case '\n':
                    escaped << "\\n";
                    break;
                case '\r':
                    escaped << "\\r";
                    break;
                case '\t':
                    escaped << "\\t";
                    break;
                default:
                    if (ch < 0x20)
                    {
                        escaped << "\\u"
                                << std::hex
                                << std::setw(4)
                                << std::setfill('0')
                                << static_cast<int>(ch)
                                << std::dec
                                << std::setfill(' ');
                    }
                    else
                    {
                        escaped << static_cast<char>(ch);
                    }
                    break;
                }
            }
            return escaped.str();
        };

        auto write_chunk_array = [&](const char* key, const std::vector<BinChunkRef>& refs)
        {
            out << "  \"" << key << "\": [\n";
            for (size_t i = 0; i < refs.size(); ++i)
            {
                const auto& ref = refs[i];
                out << "    {\"chunkIndex\":" << ref.chunk_index
                    << ",\"offset\":" << ref.offset
                    << ",\"length\":" << ref.length;
                if (!ref.which.empty())
                {
                    out << ",\"which\":\"" << escape_json_string(ref.which) << "\"";
                }
                if (!ref.lang.empty())
                {
                    out << ",\"lang\":\"" << escape_json_string(ref.lang) << "\"";
                }
                out << "}";
                if (i + 1 < refs.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ]";
        };

        out << "{\n";
        out << "  \"file\":\"" << escape_json_string(data.filename) << "\",\n";
        out << "  \"header\":{\"offset\":0,\"length\":" << data.header_length_bytes << "},\n";
        write_chunk_array("left", data.left_chunks);
        out << ",\n";
        write_chunk_array("right", data.right_chunks);
        out << ",\n";
        write_chunk_array("nameOfNode", data.name_of_node_chunks);
        out << ",\n";
        write_chunk_array("nodeOfName", data.node_of_name_chunks);
        out << "\n}\n";
    }

    static std::vector<uint32_t> parse_chunk_index_list(const std::string& value, const std::string& label)
    {
        std::vector<uint32_t> indices;
        if (value.empty() || value == "-" || value == "none")
        {
            return indices;
        }

        std::stringstream stream(value);
        std::string       token;
        while (std::getline(stream, token, ','))
        {
            if (token.empty())
            {
                continue;
            }
            const auto first_non_space = token.find_first_not_of(" \t");
            if (first_non_space != std::string::npos)
            {
                const auto last_non_space = token.find_last_not_of(" \t");
                token                     = token.substr(first_non_space, last_non_space - first_non_space + 1);
            }
            if (token.empty())
            {
                continue;
            }
            if (token == "-" || token == "none")
            {
                throw std::runtime_error("Invalid chunk selector '" + token + "' in " + label);
            }

            try
            {
                size_t pos = 0;
                if (token[0] == '-')
                {
                    throw std::runtime_error("");
                }
                uint64_t parsed = std::stoull(token, &pos, 10);
                if (pos != token.size())
                {
                    throw std::runtime_error("");
                }
                if (parsed > std::numeric_limits<uint32_t>::max())
                {
                    throw std::runtime_error("");
                }
                uint32_t index = static_cast<uint32_t>(parsed);
                indices.push_back(index);
            }
            catch (...)
            {
                throw std::runtime_error("Invalid chunk index '" + token + "' in " + label);
            }
        }

        return indices;
    }

    static std::vector<uint64_t> parse_node_id_list(const std::string& value, const std::string& label)
    {
        std::vector<uint64_t> ids;
        if (value.empty() || value == "-" || value == "none")
        {
            return ids;
        }

        std::stringstream stream(value);
        std::string       token;
        while (std::getline(stream, token, ','))
        {
            if (token.empty())
            {
                continue;
            }
            const auto first_non_space = token.find_first_not_of(" \t");
            if (first_non_space != std::string::npos)
            {
                const auto last_non_space = token.find_last_not_of(" \t");
                token                     = token.substr(first_non_space, last_non_space - first_non_space + 1);
            }
            if (token.empty())
            {
                continue;
            }
            if (token == "-" || token == "none")
            {
                throw std::runtime_error("Invalid node-route selector '" + token + "' in " + label);
            }

            try
            {
                size_t pos = 0;
                if (token[0] == '-')
                {
                    throw std::runtime_error("");
                }
                uint64_t id = std::stoull(token, &pos, 10);
                if (pos != token.size())
                {
                    throw std::runtime_error("");
                }
                ids.push_back(id);
            }
            catch (...)
            {
                throw std::runtime_error("Invalid node ID '" + token + "' in " + label);
            }
        }

        return ids;
    }
#endif

    void require_full_graph_mode(const char* command_name) const
    {
#ifndef __EMSCRIPTEN__
        if (_repl_state && _repl_state->partial_load_mode)
        {
            throw std::runtime_error("Blocked in partial load mode; full graph required for "
                                     + std::string(command_name)
                                     + ". Loaded source: "
                                     + (_repl_state->partial_load_source.empty() ? "<unknown>" : _repl_state->partial_load_source));
        }
#endif
    }

#define DEFAULT_EXCLUDE_NODES {_n->core.RelationTypeCategory, _n->core.IsA}

    void display_node_details(network::Node nd, bool resolved_from_name, int depth = 1, int max_neighbors = string::default_display_max_neighbors) const
    {
        if (resolved_from_name)
        {
            _n->out_stream() << "Resolved to node ID: " << nd << std::endl;
        }

        _n->out_stream() << "Node ID: " << nd << std::endl;

        {
            std::string core_name = _n->get_core_name(nd);
            if (!core_name.empty())
            {
                _n->out_stream() << "  Core node: " << core_name << std::endl;
            }
        }

        _n->out_stream() << "  Variable: " << (network::Network::is_var(nd) ? "yes" : "no") << std::endl;

        bool        has_wikidata = false;
        std::string wikidata_name;
        bool        has_any_name = false;

        for (const std::string& lang : _n->get_languages())
        {
            std::string name = _n->get_name(nd, lang, false);
            if (!name.empty())
            {
                has_any_name = true;
                _n->out_stream() << "  Name in language '" << lang << "': '" << name << "'" << std::endl;
                if (lang == "wikidata")
                {
                    has_wikidata  = true;
                    wikidata_name = name;
                }
            }
        }

        if (!has_any_name)
        {
            _n->out_stream() << "  (No names in any language)" << std::endl;
        }

        if (has_wikidata)
        {
            std::string       prefix    = (wikidata_name[0] == 'P') ? "Property:" : "";
            std::string       url       = "https://www.wikidata.org/wiki/" + prefix + wikidata_name;
            const std::string OSC_START = "\033]8;;";
            const char        OSC_SEP   = '\a';
            const std::string OSC_END   = "\033]8;;\a";
            _n->out_stream() << "  Wikidata URL: " << OSC_START << url << OSC_SEP << url << OSC_END << std::endl;
        }

        if (depth > 0)
        {
            generate_and_print_mermaid_link(nd,
                                            depth,
                                            max_neighbors,
                                            DEFAULT_EXCLUDE_NODES);
        }

        auto format_node = [this, max_neighbors](network::Node node) -> std::string
        {
            std::string node_str  = std::to_string(node);
            std::string node_name = _n->get_name(node, _n->lang(), true); // fallback active
            if (node_str == node_name || node_name.empty())
            {
                std::string fact_repr;
                string::node_to_string(_n, fact_repr, _n->lang(), node, max_neighbors);
                if (!fact_repr.empty() && fact_repr != "??")
                {
                    return fact_repr + " (ID " + string::unmark_identifiers(std::to_string(node)) + ")";
                }
                else
                {
                    return "ID " + string::unmark_identifiers(std::to_string(node));
                }
            }
            else
            {
                return node_name + " (ID " + string::unmark_identifiers(std::to_string(node)) + ")";
            }
        };

        auto display_connections = [&](const network::adjacency_set& conns, const std::string& header)
        {
            if (conns.empty())
            {
                return;
            }

            _n->out_stream() << "  " << header << ":" << std::endl;
            if (conns.size() <= max_neighbors)
            {
                for (network::Node node : conns)
                {
                    _n->out_stream() << "    - " << string::unmark_identifiers(format_node(node)) << std::endl;
                }
            }
            else
            {
                _n->out_stream() << "    (" << conns.size() << " connections)" << std::endl;
            }
        };

        display_connections(_n->get_left(nd), "Incoming connections from");
        display_connections(_n->get_right(nd), "Outgoing connections to");

        std::string fact_repr;
        string::node_to_string(_n, fact_repr, _n->lang(), nd, max_neighbors);
        if (!fact_repr.empty() && fact_repr != "??")
        {
            _n->out_stream() << "  Representation: " << string::unmark_identifiers(fact_repr) << std::endl;
        }

        _n->out_stream() << "------------------------" << std::endl;
    }

    void generate_and_print_mermaid_link(network::Node                            nd,
                                         int                                      depth,
                                         int                                      max_neighbors,
                                         const std::unordered_set<network::Node>& exclude_nodes,
                                         bool                                     dark_theme        = true,
                                         bool                                     horizontal_layout = true,
                                         bool                                     use_subgraphs     = true) const
    {
        std::filesystem::path temp_dir  = std::filesystem::temp_directory_path();
        std::string           hex_name  = _n->get_name_hex(nd, false, max_neighbors);
        std::string           safe_name = string::sanitize_filename(hex_name);
        std::filesystem::path html_path = temp_dir / (safe_name + ".html");

        io::gen_mermaid_html(_n,
                             nd,
                             html_path.string(),
                             depth,
                             max_neighbors,
                             exclude_nodes,
                             dark_theme,
                             horizontal_layout,
                             use_subgraphs);

        _repl_state->last_graph_html_path = html_path.string();

#ifndef __EMSCRIPTEN__
        // In a real terminal the OSC 8 link is clickable. In the wasm
        // playground the graph panel fetches the file from MEMFS via
        // take_last_graph_html() instead, and a dead file:// link in the
        // browser terminal would only mislead - so print nothing there.
        std::string file_url = "file://" + html_path.string();

        const std::string OSC_START = "\033]8;;";
        const char        OSC_SEP   = '\a';
        const std::string OSC_END   = "\033]8;;\a";

        _n->out_stream() << "  Mermaid HTML: " << OSC_START << file_url << OSC_SEP << file_url << OSC_END << std::endl;
#endif
    }

    network::Node resolve_node(const std::string& arg, const std::string& lang) const
    {
        network::Node nd = _n->get_node(arg, lang);
        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                    nd = 0;
                else if (!_n->exists(nd))
                    throw std::runtime_error("Node does not exist");
            }
            catch (...)
            {
                nd = 0;
            }
        }
        return nd;
    }

    network::Node resolve_single_node(const std::string& arg, bool prioritize_id) const
    {
        bool is_numeric = std::all_of(arg.begin(), arg.end(), ::iswdigit);

        if (is_numeric && prioritize_id)
        {
            try
            {
                size_t        pos = 0;
                network::Node nd  = std::stoull(arg, &pos);
                if (pos == arg.length() && _n->exists(nd)) return nd;
            }
            catch (...)
            {
            }
        }

        network::Node nd = _n->get_node(arg);
        if (nd != 0) return nd;

        if (is_numeric && !prioritize_id)
        {
            try
            {
                size_t        pos   = 0;
                network::Node nd_id = std::stoull(arg, &pos);
                if (pos == arg.length() && _n->exists(nd_id)) return nd_id;
            }
            catch (...)
            {
            }
        }

        throw std::runtime_error("Unknown node '" + arg + "'");
    }

public:
    void import_file(const std::string& file, const std::vector<std::string>& args = {}) const
    {
        // Resolve against the working directory first, then the standard
        // library ('.zph' extension optional). This also covers scripts
        // passed on the command line (Interactive::process_file ends up
        // here), so `zelph examples/english` works like `.import`.
        const std::string resolved = resolve_script_path(file);

        // Import guard for .zph scripts (Janet scripts are runnable programs
        // that may legitimately be executed repeatedly, e.g. with different
        // arguments -- they are exempt).
        if (std::filesystem::path(resolved).extension() != ".janet")
        {
            const std::vector<std::string> module_ids = scan_module_ids(resolved);
            const std::string&             self       = module_ids.front(); // default ID

            for (const auto& id : module_ids)
            {
                const auto it = _repl_state->imported_module_ids.find(id);
                if (it == _repl_state->imported_module_ids.end()) continue;

                if (it->second == self)
                {
                    // Plain re-import of the same script.
                    _n->diagnostic_stream() << "Skipping already imported " << resolved << std::endl;
                }
                else if (_repl_state->import_depth == 0)
                {
                    // The user explicitly asked for THIS script, but an
                    // alternative implementation already claimed the ID.
                    _n->error("Warning: skipping import of '" + file + "': module ID '" + id
                                  + "' is already provided by '" + it->second + "'",
                              true);
                }
                else
                {
                    // Nested import: an alternative provider being present is
                    // the intended substitution mechanism -- inform, don't warn.
                    _n->diagnostic_stream() << "Skipping " << resolved << ": module ID '" << id
                                            << "' is already provided by '" << it->second << "'" << std::endl;
                }
                return;
            }

            // Register BEFORE executing: a second import request -- including an
            // import cycle -- terminates immediately.
            for (const auto& id : module_ids)
                _repl_state->imported_module_ids.emplace(id, self);
        }

        // Nesting depth: suppresses the input echo inside imported scripts and
        // distinguishes direct from nested import requests (see the guard above).
        struct ImportDepthGuard
        {
            ReplState& state;
            explicit ImportDepthGuard(ReplState& s) : state(s) { ++state.import_depth; }
            ~ImportDepthGuard() { --state.import_depth; }
        } depth_guard{*_repl_state};

        AutoRunSuspender suspend(_repl_state);

        // RAII so the exception path releases suppression too; the depth
        // counter in suppress_input_capture keeps nested imports correct.
        struct CaptureSuppressor
        {
            network::Reasoning* n;
            explicit CaptureSuppressor(network::Reasoning* r) : n(r) { n->suppress_input_capture(true); }
            ~CaptureSuppressor() { n->suppress_input_capture(false); }
        } capture_guard{_n};

        _n->diagnostic_stream() << "Importing file " << resolved << "..." << std::endl;

        if (std::filesystem::path(resolved).extension() == ".janet")
        {
            // Janet scripts are executed as whole programs with janet CLI
            // semantics: fresh environment, relative imports like (use ./foo)
            // resolve against the script's directory, and a main function -
            // if defined - is called with the script path followed by args.
            // Runs inside the Janet event loop, so ev/... (threads, channels,
            // timers) is fully supported. set_script_args is not needed here:
            // the runner injects the args into the script's environment.
            _script_engine->run_janet_script(resolved, args);
        }
        else
        {
            _script_engine->set_script_args(args);

            std::ifstream stream(resolved);
            if (stream.fail()) throw std::runtime_error("Could not open file '" + resolved + "'");

            for (std::string line_utf8; std::getline(stream, line_utf8);)
            {
                _process_line_callback(line_utf8);
            }

            finish_input();
        }

        if (suspend.was_active())
        {
            _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false, true);
        }
    }

private:
    void list_predicate_usage(size_t limit)
    {
        // Map to store predicate node and its usage count
        std::map<network::Node, size_t> predicate_usage_counts;

        // Get all predicates directly: nodes that IsA RelationTypeCategory
        auto predicates = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);

        for (const auto& pred : predicates)
        {
            // Get all facts where this node is used as a relation type
            const auto& facts_using_predicate = _n->get_left(pred);
            predicate_usage_counts[pred]      = facts_using_predicate.size();
        }

        // Convert map to vector for sorting
        std::vector<std::pair<network::Node, size_t>> sorted_predicates(predicate_usage_counts.begin(), predicate_usage_counts.end());

        // Sort the predicates by usage count in ascending order
        std::sort(sorted_predicates.begin(), sorted_predicates.end(), [](const auto& a, const auto& b)
                  {
                      return a.second < b.second; // Sort by count, ascending
                  });

        // Determine if wikidata language is available for three-column output
        bool has_wikidata_lang = _n->has_language("wikidata");

        _n->out("Predicate Usage:", true);
        _n->out("------------------------", true);

        size_t total           = sorted_predicates.size();
        size_t entries_to_show = limit ? std::min(limit, total) : total;
        size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

        for (size_t i = start_idx; i < total; ++i)
        {
            const auto& entry          = sorted_predicates[i];
            std::string predicate_name = _n->get_name(entry.first, "", true); // Current language, with fallback
            std::string line_output;

            if (has_wikidata_lang && _n->get_lang() != "wikidata")
            {
                // Three columns: current lang name \t wikidata name \t count
                // For the first column, `lang` is an empty string to use the current language.
                // For the second column (wikidata name), `lang` is "wikidata" and `fallback` is `false`.
                std::string wikidata_name = _n->get_name(entry.first, "wikidata", false);
                line_output               = predicate_name + "\t" + wikidata_name + "\t" + std::to_string(entry.second);
            }
            else
            {
                // Two columns: current lang name \t count
                // `lang` is an empty string to use the current language, `fallback` is `true`.
                line_output = predicate_name + "\t" + std::to_string(entry.second);
            }
            _n->out(line_output, true);
        }
        _n->out("------------------------", true);
        if (limit && limit < total)
            _n->out("Showing top " + std::to_string(limit) + " of " + std::to_string(total) + " predicates.", true);
    }

    void list_predicate_value_usage(const std::string& pred_arg, size_t limit /*= 0*/)
    {
        // Resolve the predicate node (accept name in current language or raw numeric ID)
        network::Node pred = _n->get_node(pred_arg);
        if (pred == 0)
        {
            try
            {
                size_t pos = 0;
                pred       = std::stoull(pred_arg, &pos);
                if (pos != pred_arg.length())
                    throw std::runtime_error("");
            }
            catch (...)
            {
                throw std::runtime_error("Unknown predicate '" + pred_arg + "' in current language '" + _n->lang() + "'");
            }
        }

        std::string pred_display = _n->get_name(pred, _n->lang(), true);
        if (pred_display.empty())
            pred_display = pred_arg;

        _n->out("Value Usage for predicate " + pred_display + ":", true);
        _n->out("------------------------", true);

        ankerl::unordered_dense::map<network::Node, size_t> value_counts;

        // All fact nodes that use this predicate: fact --> pred
        network::adjacency_set facts = _n->get_left(pred);

        for (network::Node fact : facts)
        {
            network::adjacency_set incoming = _n->get_left(fact); // subject(s) + object(s) --> fact

            // Objects are incoming nodes where fact does NOT point back to them
            for (network::Node cand : incoming)
            {
                if (!_n->has_right_edge(fact, cand))
                {
                    value_counts[cand]++;
                }
            }
        }

        // Sort by count ascending
        std::vector<std::pair<size_t, network::Node>> sorted;
        sorted.reserve(value_counts.size());
        for (const auto& p : value_counts)
        {
            sorted.emplace_back(p.second, p.first);
        }
        std::sort(sorted.begin(), sorted.end());

        bool        has_wikidata_lang = _n->has_language("wikidata");
        std::string curr_lang         = _n->get_lang();

        size_t total           = sorted.size();
        size_t entries_to_show = limit ? std::min(limit, total) : total;
        size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

        for (size_t i = start_idx; i < total; ++i)
        {
            const auto& entry      = sorted[i];
            std::string value_name = _n->get_name(entry.second, "", true); // current language with fallback

            std::string line;
            if (has_wikidata_lang && curr_lang != "wikidata")
            {
                std::string wikidata_name = _n->get_name(entry.second, "wikidata", false);
                if (wikidata_name.empty())
                    wikidata_name = "(no ID)";
                line = value_name + "\t" + wikidata_name + "\t" + std::to_string(entry.first);
            }
            else
            {
                line = value_name + "\t" + std::to_string(entry.first);
            }
            _n->out(line, true);
        }

        _n->out("------------------------", true);
        _n->out("Total unique values: " + std::to_string(total), true);
        if (limit && limit < total)
            _n->out("Showing top " + std::to_string(limit) + " of " + std::to_string(total) + " values.", true);
        if (total == 0)
        {
            _n->out("(No values found for this predicate)", true);
        }
    }

    // --- Command Handlers ---

    void cmd_help(const std::vector<std::string>& cmd)
    {
        static const std::vector<std::string> general_help_lines = {
            "zelph Interactive Help",
            "",
            "Available Commands",
            "──────────────────",
            "",
            "Session",
            "  .help [command]                           – Show this help or detailed help for a specific command",
            "  .quit                                     – Exit REPL (quits zelph)",
            "  .licenses                                 – Show third-party libraries and licenses",
            "",
            "Scripts, Loading & Saving",
            "  .import <script> [args...]                – Load and execute a zelph (.zph, optional) or Janet (.janet) script; falls back to the standard library",
            "  .provides <id> [id2 ...]                  – Claim module IDs in the import registry",
#ifndef __EMSCRIPTEN__
            "  .load <file>                              – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)",
            "  .load-partial <file|manifest> [...]       – Load selected chunks as a read-only partial view (see .help .load-partial)",
            "  .save <file.bin>                          – Save the current network to a binary file",
            "  .save-predicates <file.bin> <pred> [...]  – Save only the facts of the given predicates (a slice)",
            "  .stat-file <file.bin>                     – Show serialized-file chunk statistics without loading the network",
            "  .index-file <file.bin> <json>             – Emit a JSON byte-offset index for a serialized .bin file",
#endif
            "",
            "Languages & Names",
            "  .lang [code]                              – Show or set current language (e.g. en, de, wikidata)",
            "  .name <node|id> <new_name>                – Set name in current language",
            "  .name <node|id> <lang> <new_name>         – Set name in specific language",
            "  .delname <node|id> [lang]                 – Delete name in current language (or specified language)",
            "",
            "Exploring the Network",
            "  .stat                                     – Show network statistics (nodes, RAM usage, name entries, languages, rules)",
            "  .explain [<fact>] [depth]                 – Reconstruct why a fact holds (proof tree; no arg: last output, 0 = unlimited depth); alias: .why",
            "  .list <count>                             – List first N existing nodes (internal map order, with details)",
            "  .clist <count>                            – List first N nodes named in current language (sorted by ID if feasible)",
            "  .node [<name|id>]                         – Show detailed node information; defaults to last output node",
            "  .out <name|id> [count]                    – List details of outgoing connected nodes (default 20)",
            "  .in <name|id> [count]                     – List details of incoming connected nodes (default 20)",
            "  .mermaid <node_name> [max_depth]          – Generate Mermaid HTML file for a node (default depth 3)",
            "  .list-predicate-usage [max]               – Show predicate usage statistics (top N most frequent predicates)",
            "  .list-predicate-value-usage <pred> [max]  – Show object/value usage statistics for a specific predicate (top N most frequent values)",
            "",
            "Inference & Rules",
            "  .run                                      – Run full inference",
            "  .run-once                                 – Run a single inference pass",
            "  .run-delta                                – Run inference seeded only by the facts added since the last run",
#ifndef __EMSCRIPTEN__
            "  .run-export <file>                        – Run inference and write all derivations to a JSON Lines file (see .help .run-export)",
#endif
            "  .auto-run                                 – Toggle automatic execution of .run after each input; takes no argument (default: on)",
            "  .deductions [all|focus|off]               – Set the deduction printing mode (default: focus)",
            "  .list-rules                               – List all defined inference rules",
            "  .remove-rules                             – Remove all inference rules",
            "",
            "Editing & Removing",
            "  .remove <name|id>                         – Remove a node (destructive: disconnects all edges and cleans names)",
            "  .prune-facts <pattern>                    – Remove all facts matching the query pattern (only statements)",
            "  .prune-nodes <pattern>                    – Remove matching facts AND all involved subject/object nodes",
            "  .cleanup                                  – Remove isolated nodes and clean name mappings",
            "  .new                                      – Clear the complete network and re-initialize the core nodes",
            "",
            "Clusters",
            "  .cluster [name]                           – Show clusters, or activate one ('default' = no cluster)",
            "  .cluster-drop <name>                      – Remove a cluster INCLUDING all nodes created in it (rollback)",
            "  .cluster-merge <from> <to>                – Move a cluster's membership into another ('default' = keep nodes, forget cluster)",
#ifndef __EMSCRIPTEN__
            "",
            "Wikidata",
            "  .wikidata-constraints <json> <dir>        – Export property constraints as zelph scripts to a directory",
            "  .wikidata-qualifiers <json> [P1 P2 ...]   – Import statement qualifiers from a Wikidata dump (all, or only listed qualifier properties)",
            "  .export-wikidata <json> <id1> [id2 ...]   – Extract exact JSON lines for Q-IDs (no import)",
#endif
            "",
            "Engine Behaviour",
            "  .parallel                                 – Toggle parallel processing (default: on)",
            "  .anchors [on|off]                         – Show or set anchor-based candidate lookups in unification (default: on)",
            "  .semi-naive [on|off|check]                – Show or set the fixpoint evaluation strategy (default: on)",
            "  .fact-stores [on|off]                     – Show or disable the fact-path acceleration stores (memory vs. speed)",
            "",
            "Logging & Profiling",
            "  .log <max-depth>                          – Enable detailed reasoning logging up to given recursion depth (0 = off, -1 = counters only)",
            "  .log-janet                                – Toggle logging of Janet function calls (inputs/outputs)",
            "  .prof [reset]                             – Dump reasoning profiler counters (requires .log -1 or .log N); 'reset' starts a fresh window",
            "",
            "Type \".help <command>\" for detailed information about a specific command.",
            "",
            "Basic Syntax",
            "────────────",
            "Facts:    <subject> <predicate> <object>",
            "          Predicates with spaces must be quoted.",
            "          Example: peter \"is father of\" paul",
            "",
            "Queries:  Statements containing variables (A-Z or starting with _).",
            "          Example:",
            "          _who \"is father of\" paul",
            "          Answer:  peter   is father of   paul",
            "",
            "Results:  ? <statement> asserts the statement, runs inference quietly,",
            "          then queries its result (the stdlib-wide \"= idiom\"):",
            "          ? (&17 mod &5)              Answer: (&17 mod &5) = &2",
            "          ? :testprime &53            Answer: (:testprime &53) = prime",
            "          ? $( x*x ) diffby x         Answer: ... = (x + x)",
            "          No answer means no result was derivable (partiality by absence).",
            "",
            "Rules:    (condition1, condition2, ...) => (deduction)",
            "          Example:",
            "          Berlin \"is capital of\" Germany",
            "          Germany \"is located in\" Europe",
            "            (X \"is capital of\" Y,",
            "             Y \"is located in\" Z)",
            "          => (X \"is located in\" Z)",
            "          Answer: Berlin   is located in   Europe",
            "                  ⇐ {( Germany   is located in   Europe )",
            "                      ( Berlin   is capital of   Germany )}",
            "",
            "Janet Scripting",
            "───────────────",
            "Janet:    %<code> (inline, one line) or bare % (toggle block mode until next %).",
            "          Janet generates facts/rules programmatically – then zelph inference runs as usual.",
            "          Example (using the Berlin/Germany facts from above):",
            "          %(zelph/fact \"Berlin\" \"is capital of\" \"Germany\")",
            "          Germany \"is located in\" Europe",
            "          %",
            "          (let [cond (zelph/set",
            "                      (zelph/fact 'X \"is capital of\" 'Y)",
            "                      (zelph/fact 'Y \"is located in\" 'Z))]",
            "            (zelph/fact cond \"~\" \"conjunction\")",
            "            (zelph/fact cond \"=>\" (zelph/fact 'X \"is located in\" 'Z)))",
            "          %",
            "          Answer: Berlin   is located in   Europe",
            "                  ⇐ {( Germany   is located in   Europe )",
            "                      ( Berlin   is capital of   Germany )}",
            "",
            "Unquote:  ,janet-var inside zelph lines (after defining in Janet).",
            "          Example:",
            "          %(def berlin (zelph/resolve \"Berlin\"))",
            "          ,berlin \"is capital of\" Germany"};

        static const std::map<std::string, std::string> detailed_help = {
            {".help", ".help [command]\n"
                      "Without argument: shows this general help text with syntax and command overview.\n"
                      "With argument: shows detailed help for the specified command."},

            {".quit", ".quit\n"
                      "Exits the interactive REPL session and quits zelph."},

            {".lang", ".lang [language_code]\n"
                      "Without argument: displays the current language used for node names.\n"
                      "With argument: sets the language (e.g., 'zelph', 'en', 'de', 'wikidata')."},

            {".name", ".name <node|id> <new_name>\n"
                      "Sets the name of the node in the current language.\n"
                      ".name <node|id> <lang> <new_name>\n"
                      "Sets the name in the specified language.\n"
                      "The <node|id> can be a name (in current language) or numeric node ID.\n"
                      "Empty <new_name> is not allowed – use .delname to remove a name."},

            {".delname", ".delname <node|id> [lang]\n"
                         "Removes the name of the node in the current language (or the specified language if provided).\n"
                         "The <node|id> can be a name (in current language) or numeric node ID.\n"
                         "If the node had no name in the target language, nothing happens."},

            {".list", ".list <count>\n"
                      "Lists the first N existing nodes in the network (in internal map iteration order).\n"
                      "For each node: ID, non-empty names in all languages, connection counts, representation, and Wikidata URL if available."},

            {".clist", ".clist <count>\n"
                       "Lists the first N nodes that have a name in the current language.\n"
                       "If the language has a reasonable number of entries (≤ ~50k), nodes are sorted by ID.\n"
                       "For very large languages (e.g. 'wikidata'), order follows the internal map (fast, no full sort)."},

            {".out", ".out <name|id> [count]\n"
                     "Lists detailed information for up to <count> nodes reachable via outgoing connections\n"
                     "from the given node (default 20, sorted by node ID)."},

            {".in", ".in <name|id> [count]\n"
                    "Lists detailed information for up to <count> nodes that have outgoing connections\n"
                    "to the given node (default 20, sorted by node ID)."},

            {".node", ".node [<name_or_id>]\n"
                      "Displays details for a single node: its ID, non-empty names in all languages,\n"
                      "incoming/outgoing connection counts, and a clickable Wikidata URL if it has a Wikidata ID.\n"
                      "The argument can be a name (in current language) or a numeric node ID.\n"
                      "If no argument is given, the node from the last output is used."},

            {".mermaid", ".mermaid <node_name> [max_depth]\n"
                         "Generates a Mermaid HTML file visualizing the specified node and its connections\n"
                         "up to the given depth (default 3). The file is named <node_name>.html in the system temp dir.\n"
                         "Outputs a clickable file:// link to the generated HTML."},

            {".run", ".run\n"
                     "Performs full inference: repeatedly applies all rules until no new facts are derived.\n"
                     "Deductions are printed as they are found."},

            {".run-once", ".run-once\n"
                          "Performs a single inference pass."},

            {".run-delta", ".run-delta\n"
                           "Like .run, but starts from the facts created since the previous run\n"
                           "instead of from a pass over the whole graph.\n"
                           "\n"
                           ".run always begins with one classic pass, because it cannot know what\n"
                           "the graph looked like before. That pass costs time proportional to the\n"
                           "graph, so repeatedly adding a little and running again costs time\n"
                           "proportional to everything accumulated so far, not to what was added.\n"
                           ".run-delta removes that term: it seeds the fixpoint with the new facts\n"
                           "and lets semi-naive evaluation take it from there.\n"
                           "\n"
                           "This is only equivalent to .run when the graph already is a fixpoint of\n"
                           "the current rules -- otherwise the missing pass is exactly the one that\n"
                           "would have found the older consequences. The command therefore falls\n"
                           "back to a full pass, with a note, unless a previous run has happened,\n"
                           "the rule set is unchanged since, and .semi-naive is on.\n"
                           "\n"
                           "Typical use is a program driving zelph as a library: run once to\n"
                           "saturate, then assert and .run-delta per unit of new data."},

            {".explain", ".explain [<fact pattern>] [max-depth]   (alias: .why)\n"
                         "Reconstructs a justification for an asserted fact from the graph\n"
                         "and prints it as a proof tree in the '⇐' notation. Without a\n"
                         "pattern, the last output node is explained -- the natural\n"
                         "companion of the '?' prefix:\n"
                         "    .import binary-arithmetic\n"
                         "    ? (&6 + &7)\n"
                         "    .explain\n"
                         "Nothing is recorded during inference: after quiescence, every\n"
                         "derived fact has a rule instantiation whose conditions are all\n"
                         "present, and .explain finds one by backward search (forward\n"
                         "chaining keeps full provenance in the graph itself). Leaves are\n"
                         "marked [axiom] (input facts); negation-as-failure premises show\n"
                         "as ¬(...) [absent], verified against the CURRENT graph. A shared\n"
                         "DERIVED subproof is expanded once and referenced afterwards\n"
                         "([see above]); repeated axioms stay written out, since [axiom]\n"
                         "is already their complete expansion.\n"
                         "max-depth defaults to 3; 0 means unlimited. If several\n"
                         "justifications exist, one is shown. Term islands work inside\n"
                         "the pattern: .explain $( x*x ) diffby x = D is invalid, but\n"
                         ".explain ($( x*x ) diffby x) = (x + x) resolves as usual."},
#ifndef __EMSCRIPTEN__
            {".run-export", ".run-export <file>\n"
                            "Performs full inference and writes every derived fact and contradiction to\n"
                            "<file> as JSON Lines -- one object per line:\n"
                            "    {\"kind\":\"deduction\",\"conclusion\":[SEG,...],\"premises\":[[SEG,...],...]}\n"
                            "A SEG is either a JSON string (literal text of the rendering) or an object\n"
                            "{\"names\":{\"<lang>\":\"<name>\",...}} naming one node in every language it is\n"
                            "known by. Nothing in the file is specific to a target format: which name to\n"
                            "show, what to link, how to group -- all of that is the converter's decision.\n"
                            "dev_scripts/zelph-derivations.py --format md turns the file into the MkDocs\n"
                            "reports zelph used to write directly; --format text gives one flat line\n"
                            "per derivation, the starting point for tokenizer-friendly training data.\n"
                            "Deduction printing is off during the run: rendering to the console dominates\n"
                            "the cost, and the file is the point."},
#endif
            {".list-rules", ".list-rules\n"
                            "Lists all currently defined inference rules in readable format."},

            {".list-predicate-usage", ".list-predicate-usage [max_entries]\n"
                                      "Shows how often each predicate (relation type) is used, sorted by frequency.\n"
                                      "If <max_entries> is specified, only the top N most frequent predicates are shown.\n"
                                      "If Wikidata language is active, Wikidata IDs are shown alongside names."},

            {".list-predicate-value-usage", ".list-predicate-value-usage <predicate> [max_entries]\n"
                                            "Shows how often each object (value) is used with the specified predicate, sorted by frequency.\n"
                                            "The <predicate> can be a name (in the current language) or a numeric node ID.\n"
                                            "If <max_entries> is specified, only the top N most frequent values are shown.\n"
                                            "If the Wikidata language is available and active, Wikidata IDs are shown alongside names."},

            {".remove-rules", ".remove-rules\n"
                              "Deletes all inference rules from the network."},

            {".remove", ".remove <name_or_id>\n"
                        "Removes the specified node from the network, disconnecting all its edges\n"
                        "and cleaning all name mappings. The argument can be a node name (looked up in the current language)\n"
                        "or a numeric node ID.\n"
                        "WARNING: This operation is destructive and irreversible!"},

            {".import", ".import <script> [args...]\n"
                        "Loads and immediately executes a script. Two script types are supported:\n"
                        "  .zph   – zelph scripts, processed line by line. The extension is optional.\n"
                        "  .janet – Janet programs, run like the janet CLI would: fresh environment\n"
                        "           with the zelph/... API available, relative imports such as\n"
                        "           (use ./foo) resolve against the script's directory, ev/... (threads,\n"
                        "           channels) works, and a main function - if defined - is called.\n"
                        "           The '.janet' extension must be spelled out.\n"
                        "\n"
                        "Anything after the script path is passed to the script as arguments:\n"
                        "  Janet scripts receive them as parameters of main (preceded by the script\n"
                        "  path, CLI convention) and via (dyn :args).\n"
                        "  zelph scripts can read them from Janet code via (dyn :args).\n"
                        "\n"
                        "Resolution order:\n"
                        "  1. The path as given (absolute, or relative to the current working directory).\n"
                        "  2. The zelph standard library. Search locations, in order:\n"
                        "       - $ZELPH_STDLIB (if set)\n"
                        "       - 'stdlib' next to the zelph executable (release archives, build tree)\n"
                        "       - '../share/zelph' relative to the executable (e.g. /usr/share/zelph)\n"
                        "       - /usr/local/share/zelph and /usr/share/zelph (non-Windows)\n"
                        "\n"
                        "Standard library scripts are addressed without path or extension:\n"
                        "  .import sparql\n"
                        "  .import decimal-arithmetic\n"
                        "Subdirectories must be given explicitly:\n"
                        "  .import examples/english\n"
                        "  .import examples/neural/nn-wikidata-demo"},

            {".provides", ".provides <id> [id2 ...]\n"
                          "Claims one or more module IDs (lowercased) in the import registry.\n"
                          "Placed at the top of a script, it declares IDs the script provides\n"
                          "IN ADDITION to its default ID (its lowercase file name without\n"
                          "extension). Scripts claiming the same ID are interchangeable,\n"
                          "mutually exclusive implementations of one capability: whichever is\n"
                          "imported first wins; importing another provider of that ID is\n"
                          "skipped. Example: the arithmetic substrates (decimal-arithmetic,\n"
                          "binary-arithmetic, binary-nand-arithmetic) all declare\n"
                          "'.provides arithmetic' -- loading one satisfies every dependent\n"
                          "script's '.import binary-arithmetic' line, and loading a second\n"
                          "substrate is prevented.\n"
                          "Used interactively, .provides claims an ID without loading\n"
                          "anything, blocking all scripts that provide it.\n"
                          "The registry is cleared by .new."},
#ifndef __EMSCRIPTEN__
            {".load", ".load <file>\n"
                      "Loads a previously saved network state.\n"
                      "- If <file> ends with '.bin': loads the serialized network directly (fast).\n"
                      "- If <file> ends with '.json' or '.json.bz2' (Wikidata dump): imports the data and automatically creates a '.bin' cache file\n"
                      "  in the same directory for faster future loads."},

            {".load-partial", ".load-partial <file.bin|manifest.json> [selectors...] [options...]\n"
                              "\n"
                              "Load selected chunks from a serialized .bin file or from a JSON manifest\n"
                              "that references chunk locations (local paths or remote URIs).\n"
                              "The result is a read-only, incomplete graph view.\n"
                              "Inference (.run), pruning, cleanup, and destructive edits are blocked.\n"
                              "\n"
                              "Chunk selectors (comma-separated indices, default: load all):\n"
                              "  left=0,1,2          – load only left-adjacency chunks 0, 1, 2\n"
                              "  right=5,6           – load only right-adjacency chunks 5, 6\n"
                              "  nameOfNode=0,1      – load only name-of-node chunks 0, 1  (alias: name=)\n"
                              "  nodeOfName=0,1      – load only node-of-name chunks 0, 1  (alias: node-name=)\n"
                              "  <section>=none      – skip that section entirely (also accepts '-')\n"
                              "  (omit a selector to load all chunks of that section)\n"
                              "\n"
                              "Use .index-file to discover chunk indices and byte offsets,\n"
                              "and .stat-file for a quick chunk count overview.\n"
                              "\n"
                              "Route selectors (manifest mode only, require a node route index):\n"
                              "  route-node=<id,...>  – resolve node IDs to the chunks that contain them\n"
                              "                        (sets left, right, and nameOfNode selectors)\n"
                              "  route-name=<name>    – resolve a name to the nodeOfName chunk that contains it\n"
                              "  route-lang=<lang>    – language for route-name lookup (required with route-name)\n"
                              "  Route selectors determine which chunks to load automatically based on\n"
                              "  the manifest's node route index sidecar. They can be combined with\n"
                              "  explicit chunk selectors.\n"
                              "\n"
                              "Options:\n"
                              "  meta-only           – load only the header; skip all chunk payloads\n"
                              "\n"
                              "Manifest mode (for sharded/remote storage):\n"
                              "  Pass a .json manifest as the first argument, or use manifest=<path>.\n"
                              "  source-bin=<path>   – override the .bin path used for the header\n"
                              "  shard-root=<path>   – where the shards are, if not next to the manifest\n"
                              "  A shard is looked up next to the manifest first, then below shard-root,\n"
                              "  and only fetched (hf:// or https://) when no local copy exists; the same\n"
                              "  applies to the .bin the manifest names. A downloaded artifact therefore\n"
                              "  loads offline without further options.\n"
                              "\n"
                              "Examples:\n"
                              "  .load-partial data.bin                          – load everything (partial mode)\n"
                              "  .load-partial data.bin left=0,1 right=none      – two left chunks, no right\n"
                              "  .load-partial data.bin meta-only                 – header only, no graph data\n"
                              "  .load-partial manifest.json shard-root=/data/shards\n"
                              "  .load-partial manifest.json route-node=1        – load only chunks containing node 1\n"
                              "  .load-partial manifest.json route-name=A route-lang=wikidata"},

            {".save", ".save <file.bin>\n"
                      "Saves the current network state to a binary file.\n"
                      "The filename must end with '.bin'."},

            {".save-predicates", ".save-predicates <file.bin> <predicate> [<predicate> ...]\n"
                                 "\n"
                                 "Saves a SLICE of the current network: the facts of the named predicates,\n"
                                 "the nodes those facts connect, and all names of those nodes. Everything\n"
                                 "else is left behind. Predicates are named in the current language\n"
                                 "(.lang) or given as node IDs.\n"
                                 "\n"
                                 "The result is an ordinary network file. Loading it needs a fraction of\n"
                                 "the memory of the source and answers the same questions as long as they\n"
                                 "only involve those predicates -- which is what makes a large graph\n"
                                 "usable on a small machine.\n"
                                 "\n"
                                 "Relation-type declarations and the structure of nested facts travel\n"
                                 "with the slice automatically; a fact of another predicate between two\n"
                                 "retained nodes does not.\n"
                                 "\n"
                                 "Example (the Wikidata class hierarchy alone):\n"
                                 "  .lang wikidata\n"
                                 "  .save-predicates wikidata-20260309-P279.bin P279"},
#endif
            {".prune-facts", ".prune-facts <pattern>\n"
                             "Removes only the matching facts (statement nodes).\n"
                             "The pattern may contain variables in any position; a pattern WITHOUT\n"
                             "variables denotes one specific fact and removes exactly that one.\n"
                             "A pattern that matches nothing changes nothing -- in particular it does\n"
                             "not create the fact it describes.\n"
                             "Reports how many facts were removed."},

            {".prune-nodes", ".prune-nodes <pattern>\n"
                             "Removes all matching facts AND the nodes bound to the pattern's variable.\n"
                             "Requirements:\n"
                             "- The relation (predicate) must be fixed (no variable in predicate position)\n"
                             "- EXACTLY ONE variable, in subject or object position: it names what gets\n"
                             "  deleted. Two variables are rejected rather than silently read as one.\n"
                             "WARNING: This is highly destructive! The deleted nodes lose ALL of their\n"
                             "connections, including facts that have nothing to do with the pattern,\n"
                             "and their names go with them.\n"
                             "Relation nodes left isolated by the deletion are removed by .cleanup.\n"
                             "Reports removed facts and nodes."},

            {".cleanup", ".cleanup\n"
                         "Removes all nodes that have no connections (isolated nodes).\n"
                         "Also cleans up associated entries in name mappings."},

            {".new", ".new\n"
                     "Clears the complete network, including node names. Re-initializes core nodes.\n"
                     "Also clears the import registry, so all scripts can be imported again."},

            {".stat", ".stat\n"
                      "Shows current network statistics:\n"
                      "- Number of nodes\n"
                      "- RAM usage (in GiB, if available)\n"
                      "- Total entries in name-of-node mappings\n"
                      "- Total entries in node-of-name mappings\n"
                      "- Number of languages\n"
                      "- Number of rules"},
#ifndef __EMSCRIPTEN__
            {".stat-file", ".stat-file <file.bin>\n"
                           "Reads only the serialized zelph header from the given .bin file and prints\n"
                           "file size and chunk counts for left/right adjacency and name maps.\n"
                           "Does not load the network into memory."},

            {".index-file", ".index-file <file.bin> <output.json>\n"
                            "Scans a serialized zelph .bin file sequentially and emits a JSON sidecar\n"
                            "containing byte offsets and lengths for the header and each chunk section.\n"
                            "Does not load the graph into the live network."},
#endif
            {".licenses", ".licenses\n"
                          "Lists all third-party software embedded in zelph, including their versions and licenses."},

            {".log", ".log <max-depth>\n"
                     "Enables detailed reasoning logging up to the given recursion depth.\n"
                     "-1 collects counters without log output - dump them with .prof\n"
                     "0 disables it.\n"
                     "Every line is correctly indented according to depth."},

            {".log-janet", ".log-janet\n"
                           "Toggles detailed logging of inputs and outputs for all zelph/* Janet functions.\n"
                           "Logs inputs at function entry and both inputs and output at exit."},

            {".prof", ".prof [reset]\n"
                      "Dumps the reasoning profiler on demand: the counter block plus the\n"
                      "top-10 relations by scanned candidate facts, relations by successful\n"
                      "matches, rules by application count, and rules by created facts.\n"
                      "Counters only accumulate while logging is active; the counter-only\n"
                      "mode \".log -1\" enables them WITHOUT any per-deduction log output.\n"
                      "While logging is active, counters accumulate across statements and\n"
                      "imports (the per-statement epoch reset is deliberately a no-op\n"
                      "then), so \".prof\" after an import shows the totals for the whole\n"
                      "import. \".prof reset\" additionally zeroes the counters, starting\n"
                      "a fresh measurement window -- use it between two phases."},

            {".auto-run", ".auto-run\n"
                          "Toggles the automatic execution of the inference engine (.run) after every input.\n"
                          "Default is ON. Automatically switches to OFF when .load is used.\n"
                          "It is a TOGGLE and takes no argument -- unlike its neighbours .anchors,\n"
                          ".semi-naive and .fact-stores, which read [on|off]. \".auto-run off\" is\n"
                          "therefore an error rather than a way to switch it off twice."},

            {".deductions", ".deductions [all|focus|off]\n"
                            "Sets the deduction printing mode for reasoning runs; without an\n"
                            "argument, shows the current mode (default: focus).\n"
                            "  all    print every deduction (full derivation trace)\n"
                            "  focus  print only deductions about your own input: the deduced\n"
                            "         fact's subject stems from an interactively entered\n"
                            "         statement -- it is the entered fact itself, or its subject\n"
                            "         or one of its objects. Anchors accumulate over the session;\n"
                            "         imported scripts (.import) do not contribute. Switching\n"
                            "         modes resets the collected anchors.\n"
                            "  off    print no deductions\n"
                            "All facts are derived and stored regardless of the mode. Query\n"
                            "answers, contradictions and warnings are always printed. Filtered\n"
                            "deductions are counted in \"(skipped N deductions)\". Heavy\n"
                            "computations run several times faster with focus/off: rendering\n"
                            "large derived terms dominates the cost."},

            {".parallel", ".parallel\n"
                          "Toggles parallel processing on/off.\n"
                          "Default is on for performance."},

            {".anchors", ".anchors [on|off]\n"
                         "Controls the unification engine's anchor-based candidate lookups:\n"
                         "subject/object-driven snapshots, grounded-subject anchoring, and\n"
                         "partial-pattern anchoring. Without argument: shows the current state.\n"
                         "  on   – (default) conditions with a concrete anchor node scan only that\n"
                         "         node's adjacency instead of the whole relation extent. This is a\n"
                         "         pure index shortcut: results are identical to 'off'.\n"
                         "  off  – every condition scans the full relation extent (naive reference\n"
                         "         mode). Used by tests as an anchor-free completeness reference and\n"
                         "         for diagnosing suspected anchor bugs. Expect drastic slowdowns on\n"
                         "         arithmetic workloads.\n"
                         "Independent of .parallel: anchoring is active in both parallel and\n"
                         "single-core evaluation."},

            {".semi-naive", ".semi-naive [on|off|check]\n"
                            "Controls the fixpoint evaluation strategy of the reasoning engine.\n"
                            "Without argument: shows the current mode.\n"
                            "  on    – (default) delta-driven semi-naive evaluation: after a classic\n"
                            "          first pass, each further iteration evaluates rules only against\n"
                            "          the facts created in the previous iteration. Results are\n"
                            "          identical to 'off'; typically much faster on rule-heavy\n"
                            "          workloads such as the arithmetic modules.\n"
                            "  off   – classic naive evaluation: every iteration re-evaluates all\n"
                            "          rules against the whole graph.\n"
                            "  check – like 'on', but after the delta drains, classic verification\n"
                            "          passes run until quiescence. If any of them derives a fact the\n"
                            "          delta path missed, the run completes the fixpoint classically\n"
                            "          and then fails with a completeness-violation error. Intended\n"
                            "          for tests and debugging; the test suite always enables it.\n"
                            "Single-pass runs (.run-once) and queries are unaffected by this setting."},

            {".fact-stores", ".fact-stores [on|off]\n"
                             "Shows or disables the fact-path acceleration stores: the genuine-\n"
                             "structure store (exact triple per created fact node) and the\n"
                             "template-variable store (exact variable set per rule-template node).\n"
                             "They make structure and variable lookups O(1) but grow by roughly\n"
                             "150 bytes per fact created through the normal fact() path.\n"
                             "  (no argument) – show the current state\n"
                             "  off – disarm and free the stores; all lookups fall back to the\n"
                             "        slower, semantically identical reconstruction walks. Use\n"
                             "        BEFORE bulk-building large graphs through the fact() API,\n"
                             "        e.g. a Janet mass importer. Trusted imports and binary\n"
                             "        loads (.load) disable the stores automatically.\n"
                             "  on  – valid only while the stores are still enabled: they cannot\n"
                             "        be re-armed retroactively, because ABSENCE of an entry is\n"
                             "        meaningful while a store is authoritative.\n"
                             "Restart zelph to start with stores enabled again."},
#ifndef __EMSCRIPTEN__
            {".wikidata-constraints", ".wikidata-constraints <json_file> <output_dir>\n"
                                      "Processes the Wikidata dump and exports constraint scripts\n"
                                      "to the specified output directory."},

            {".wikidata-qualifiers", ".wikidata-qualifiers <wikidata-dump.json[.bz2]> [P-id1 P-id2 ...]\n"
                                     "Imports statement qualifiers from a Wikidata JSON dump into the current network.\n"
                                     "Without P-ids: imports all qualifiers. With P-ids: imports only qualifiers whose\n"
                                     "property is listed (e.g. P11260); a statement is only materialized if it\n"
                                     "contributes at least one matching qualifier.\n"
                                     "\n"
                                     "For each such statement, reified structures are created as ordinary nodes and\n"
                                     "facts in the 'wikidata' language (matching the RDF statement layer):\n"
                                     "  <subject>   p:<P>          <statement>   – links entity to its statement node\n"
                                     "  <statement> ps:<P>         <main value>  – the statement's main value\n"
                                     "  <statement> pq:<Pq>        <value>       – one fact per imported qualifier\n"
                                     "  <statement> wikibase:rank  wikibase:{Normal|Preferred|Deprecated}Rank\n"
                                     "Statement nodes are named by their Wikidata statement ID (contains '$').\n"
                                     "\n"
                                     "IMPORTANT: Load the base network first (.load <all.bin>), so that subjects and\n"
                                     "entity values attach to the existing Q/P nodes via their 'wikidata' names.\n"
                                     "The import is idempotent and incremental: re-running it (e.g. with additional\n"
                                     "qualifier properties) extends the loaded network. Persist the combined result\n"
                                     "with .save <file.bin>.\n"
                                     "\n"
                                     "Value handling: entity values use their Q/P ID; time, quantity, string and\n"
                                     "monolingual text values become nodes named by their raw value (e.g.\n"
                                     "'+2020-01-01T00:00:00Z', '+42'). novalue/somevalue and coordinates are skipped.\n"
                                     "\n"
                                     "Example (disjointness analysis, 'list item' qualifiers on P2738):\n"
                                     "  .load wikidata-20260309-all.bin\n"
                                     "  .wikidata-qualifiers wikidata-20260309-all.json.bz2 P11260\n"
                                     "  .save wikidata-20260309-all-P11260.bin"},

            {".export-wikidata", ".export-wikidata <wikidata-dump.json> <Qid1> [Qid2 ...]\n"
                                 "Extracts the exact JSON line for each given Wikidata ID (Q…)\n"
                                 "from the dump and writes it to <id>.json in the current directory.\n"
                                 "The dump can be .json or .json.bz2.\n"
                                 "No import, no .bin cache, no network – pure extraction."},
#endif
            {".cluster", ".cluster [name]\n"
                         "Without argument: lists all clusters with node counts and shows the active one.\n"
                         "With argument: activates the named cluster (created if needed). All nodes and\n"
                         "facts created from now on are recorded in it — including relation nodes,\n"
                         "rule definitions, query patterns, and facts deduced by .run. Facts that\n"
                         "already existed before are never recorded.\n"
                         "'.cluster default' deactivates cluster tracking.\n"
                         "Note: clusters are session state and are not persisted by .save."},

            {".cluster-drop", ".cluster-drop <name>\n"
                              "Removes every node recorded in the cluster, including all edges and names\n"
                              "(rollback semantics). Pre-existing knowledge is untouched, but facts created\n"
                              "OUTSIDE the cluster that reference cluster nodes lose those connections.\n"
                              "Dropping the ACTIVE cluster falls back to 'default', i.e. tracking stops.\n"
                              "An unknown name is an error, not a no-op.\n"
                              "WARNING: destructive and irreversible."},

            {".cluster-merge", ".cluster-merge <from> <to>\n"
                               "Moves the membership bookkeeping of <from> into <to> (commit semantics).\n"
                               "No nodes or edges are touched. If <to> is 'default', the nodes simply\n"
                               "become ordinary nodes."},
        };

        if (cmd[0] == ".help")
        {
            if (cmd.size() == 1)
            {
                for (const auto& l : general_help_lines)
                    _n->out(l, true);
            }
            else if (cmd.size() == 2)
            {
                std::string topic = cmd[1];
                if (const auto alias = command_aliases.find(topic); alias != command_aliases.end())
                    topic = alias->second;

                auto it = detailed_help.find(topic);
                if (it != detailed_help.end())
                {
                    _n->out(it->second, true);
                }
                else
                {
                    _n->error("Unknown command: " + cmd[1] + ". Use \".help\" for a list of all commands.", true);
                }
            }
            else
            {
                throw std::runtime_error("Usage: .help [command]");
            }
        }
    }
    void cmd_lang(const std::vector<std::string>& cmd)
    {
        if (cmd.size() > 2) throw std::runtime_error("Usage: .lang [code]");

        if (cmd.size() < 2)
        {
            _n->out_stream() << "The current language is '" << _n->get_lang() << "'" << std::endl;
        }
        else
        {
            _n->set_lang(cmd[1]);
        }
    }

    void cmd_name(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".name");
        if (cmd.size() < 3 || cmd.size() > 4)
            throw std::runtime_error("Command .name: Invalid arguments. Usage: .name <node> <new_name>  or  .name <node> <lang> <new_name>");

        const std::string& name_in_current_lang = cmd[1];
        const std::string& name_in_target_lang  = cmd.size() == 3 ? cmd[2] : cmd[3];
        std::string        current_lang         = _n->get_lang();
        std::string        target_lang          = cmd.size() == 3 ? _n->lang() : cmd[2];

        network::Node node_in_current_lang = resolve_node(name_in_current_lang, current_lang);
        network::Node node_in_target_lang  = resolve_node(name_in_target_lang, target_lang);

        if (current_lang == target_lang)
        {
            // In this case, name_in_current_lang is strictly interpreted as the old name that we use to reference
            // the existing node. It does not make sense to support creating a new node in this mode.
            if (node_in_current_lang == 0)
            {
                throw std::runtime_error("Node '" + name_in_current_lang + "' does not exist");
            }
            else if (node_in_target_lang == node_in_current_lang)
            {
                // Renaming a node to the name it already carries. Refusing
                // it complained about the node itself -- "Name 'a' is
                // already in use by node 11" where 11 IS 'a'.
                _n->out("Node '" + name_in_current_lang + "' already has this name in language '" + target_lang + "'.", true);
            }
            else if (node_in_target_lang != 0)
            {
                throw std::runtime_error("Name '" + name_in_target_lang + "' is already in use by node " + std::to_string(node_in_target_lang)
                                         + ". Names are unique per language; remove the other node or use a different name.");
            }
            else
            {
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
            }
        }
        else if (node_in_current_lang == 0)
        {
            if (node_in_target_lang == 0)
            {
                node_in_current_lang = _n->node(name_in_current_lang);
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
                _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') / '" + name_in_target_lang + "' ('" + target_lang + "') does not exist yet in either language => created it.", true);
            }
            else
            {
                _n->set_name(node_in_target_lang, name_in_current_lang, current_lang, true);
                _n->out("Node '" + name_in_target_lang + "' ('" + target_lang + "') exists, assigned name '" + name_in_current_lang + "' in '" + current_lang + "'.", true);
            }
        }
        else if (node_in_target_lang == 0)
        {
            _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
            _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') exists, assigned name '" + name_in_target_lang + "' in '" + target_lang + "'.", true);
        }
        else if (name_in_current_lang == _n->get_name(node_in_current_lang, current_lang, false) && name_in_target_lang == _n->get_name(node_in_target_lang, target_lang, false))
        {
            _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') / '" + name_in_target_lang + "' ('" + target_lang + "') have the requested names, but are different nodes => Merging them.", true);
            _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
        }
        else
        {
            throw std::runtime_error("Node '" + name_in_current_lang + "' ('" + current_lang + "') / '" + name_in_target_lang + "' ('" + target_lang + "') exists in both languages as different nodes => did not do anything)");
        }
    }
    void cmd_delname(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".delname");
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .delname: Invalid arguments. Usage: .delname <node|id> [lang]");

        network::Node nd = resolve_single_node(cmd[1], true); // prioritize ID

        std::string target_lang = _n->lang();
        if (cmd.size() == 3)
        {
            target_lang = cmd[2];
        }

        _n->remove_name(nd, target_lang);

        _n->out("Removed name of node " + std::to_string(nd) + " in language '" + target_lang + "' (if it existed).", true);
    }
    void cmd_node(const std::vector<std::string>& cmd)
    {
        if (cmd.size() > 2) throw std::runtime_error("Command .node: At most one argument required");

        std::string                arg;
        std::vector<network::Node> nodes;

        if (cmd.size() == 1)
        {
            network::Node last = string::last_node_to_string_node();
            if (last == network::Node{}) throw std::runtime_error("Command .node: No argument given and no previous output node available");
            nodes.push_back(last);
        }
        else
        {
            arg = cmd[1];

            try
            {
                // Try single resolve (non-destructive: ID last)
                network::Node single = resolve_single_node(arg, false);
                nodes.push_back(single);
            }
            catch (...)
            {
                // Not a single node/ID → try name search (multiple possible)
                nodes = _n->resolve_nodes_by_name(arg);
                if (nodes.empty())
                {
                    throw std::runtime_error("No node found with name '" + arg + "' in current language '" + _n->lang() + "'");
                }
            }
        }

        if (nodes.size() == 1)
        {
            bool resolved_from_name = !arg.empty() && (!_n->get_name(nodes[0], _n->lang(), false).empty() || std::all_of(arg.begin(), arg.end(), ::iswdigit));
            display_node_details(nodes[0], resolved_from_name && nodes.size() == 1);
        }
        else
        {
            // nodes.size() > 1 only reachable via name search (arg non-empty)
            _n->out_stream() << "Found " << nodes.size() << " nodes with name '" << arg
                             << "' in current language '" << _n->lang() << "':" << std::endl;
            _n->out_stream() << "------------------------" << std::endl;

            // Sort by ID for consistent output
            std::sort(nodes.begin(), nodes.end());

            for (network::Node nd : nodes)
            {
                display_node_details(nd, true);
            }
        }
    }
    void cmd_list(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .list: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_all_nodes_view();

        _n->out_stream() << "Listing " << count << " nodes:" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->first, false);
        }

        _n->out_stream() << "Displayed " << displayed << " nodes." << std::endl;
    }
    void cmd_clist(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .clist: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_lang_nodes_view(_n->lang());

        _n->out_stream() << "Listing first " << count << " nodes named in current language '" << _n->lang() << "'" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->second, false);
        }
    }
    void cmd_connections(const std::vector<std::string>& cmd, bool outgoing)
    {
        if (cmd.size() < 2) throw std::runtime_error(std::string("Command ") + cmd[0] + ": Missing node argument");

        const std::string& arg     = cmd[1];
        network::Node      base_nd = resolve_node(arg, _n->lang()); // same resolve logic as .node/.remove

        if (base_nd == 0)
        {
            throw std::runtime_error("Unknown node");
        }

        size_t max_count = 20; // default
        if (cmd.size() >= 3)
        {
            max_count = string::parse_count(cmd[2]);
        }

        network::adjacency_set neighbors = outgoing ? _n->get_right(base_nd) : _n->get_left(base_nd);

        std::vector<network::Node> vec(neighbors.begin(), neighbors.end());
        std::sort(vec.begin(), vec.end());

        size_t to_display = std::min(max_count, vec.size());

        _n->out_stream() << (outgoing ? "Outgoing" : "Incoming")
                         << " connected nodes of " << base_nd
                         << " (first " << to_display << " of " << vec.size() << ", sorted by ID):" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        for (size_t i = 0; i < to_display; ++i)
        {
            display_node_details(vec[i], false);
        }
    }
    void cmd_remove(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".remove");

        if (cmd.size() != 2) throw std::runtime_error("Command .remove requires exactly one argument: name or ID");

        const std::string& arg = cmd[1];
        network::Node      nd  = resolve_single_node(arg, true); // prioritize ID

        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                {
                    throw std::exception();
                }
            }
            catch (const std::exception&)
            {
                throw std::runtime_error("Command .remove: Unknown node '" + arg + "' in current language '" + _n->lang() + "'");
            }

            if (!_n->exists(nd))
            {
                throw std::runtime_error("Command .remove: Node '" + std::to_string(nd) + "' does not exist");
            }
        }

        _n->remove_node(nd);
        _n->out("Removed node " + std::to_string(nd) + " (all edges disconnected, name mappings cleaned).", true);
        _n->diagnostic("Consider running .cleanup afterwards if needed.", true);
    }
    void cmd_mermaid(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .mermaid: Missing node name to visualise");
        const std::string& arg = cmd[1];
        network::Node      nd  = resolve_single_node(arg, true);
        if (nd == 0) throw std::runtime_error("Command .mermaid: Unknown node '" + arg + "'");
        int max_depth     = 1;
        int max_neighbors = string::default_display_max_neighbors;
        if (cmd.size() >= 3)
        {
            max_depth = std::stoi(cmd[2]);
            if (max_depth < 1) throw std::runtime_error("Command .mermaid: Maximum depth must be greater than 0. Note: when using 1, a dynamic depth based on the node count will be used.");
        }
        if (cmd.size() >= 4)
        {
            max_neighbors = std::stoi(cmd[3]);
            if (max_neighbors < 1) throw std::runtime_error("Command .mermaid: Maximum neighbors must be at least 1");
        }
        generate_and_print_mermaid_link(nd,
                                        max_depth,
                                        max_neighbors,
                                        DEFAULT_EXCLUDE_NODES);
    }
    void cmd_run(const std::vector<std::string>&)
    {
        require_full_graph_mode(".run");
        _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false);
        _n->diagnostic("Ready.", true);
    }
    void cmd_run_once(const std::vector<std::string>&)
    {
        require_full_graph_mode(".run-once");
        _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, true);
        _n->diagnostic("Ready.", true);
    }
    void cmd_run_delta(const std::vector<std::string>&)
    {
        require_full_graph_mode(".run-delta");
        _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false, false, true);
        _n->diagnostic("Ready.", true);
    }
#ifndef __EMSCRIPTEN__
    void cmd_run_export(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".run-export");
        if (cmd.size() != 2)
            throw std::runtime_error("Command .run-export requires exactly one argument: the output file path");

        _n->set_export_file(cmd[1]);
        _n->diagnostic("Running full inference; derivations are written to " + cmd[1] + " as JSON Lines.", true);

        // Rendering every derived term to the console dominates the cost of
        // a large export, and the file is the point of the command.
        if (_data_manager) _data_manager->set_logging(false);

        _n->run(false, true, false);
        _n->diagnostic("Ready.", true);
    }
    void cmd_load(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .load: Missing bin or json file name");
        if (cmd.size() > 2) throw std::runtime_error("Command .load: Unknown argument after file name");

        if (_repl_state->auto_run)
        {
            _repl_state->auto_run = false;
            _n->out("Auto-run has been disabled due to loading a large dataset.", true);
        }

        if (cmd.size() == 2)
        {
            chrono::StopWatch watch;
            watch.start();

            // This detects if it's Wikidata (json/bz2 OR bin with source) or Generic (bin only)
            _data_manager = io::DataManager::create(_n, cmd[1]);
            _data_manager->load();
            _repl_state->partial_load_mode   = false;
            _repl_state->partial_load_source = "";

            watch.stop();
            _n->diagnostic(" Time needed for loading/importing: " + watch.format(), true);
        }
        else
        {
            throw std::runtime_error("Command .load: You need to specify one argument: the *.bin or *.json file to import");
        }
    }
    void cmd_load_partial(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2)
            throw std::runtime_error("Command .load-partial: Missing .bin file name or manifest");

        const std::string& first_arg          = cmd[1];
        bool               use_manifest       = !first_arg.ends_with(".bin");
        std::string        source_or_manifest = first_arg;
        std::string        source_bin_override;
        std::string        shard_root;

        if (!use_manifest && !std::filesystem::exists(first_arg))
        {
            throw std::runtime_error("Command .load-partial: Cannot open input file '" + first_arg + "'");
        }

        network::Zelph::BinChunkSelection selection;
        bool                              meta_only = false;

        for (size_t i = 2; i < cmd.size(); ++i)
        {
            const std::string& arg = cmd[i];
            if (arg == "meta-only")
            {
                meta_only = true;
                continue;
            }

            auto eq = arg.find('=');
            if (eq == std::string::npos)
            {
                throw std::runtime_error("Command .load-partial: Unknown argument '" + arg + "'");
            }

            std::string key   = arg.substr(0, eq);
            std::string value = arg.substr(eq + 1);

            if (key == "left")
            {
                selection.left          = parse_chunk_index_list(value, "left");
                selection.left_explicit = true;
            }
            else if (key == "right")
            {
                selection.right          = parse_chunk_index_list(value, "right");
                selection.right_explicit = true;
            }
            else if (key == "nameOfNode" || key == "name")
            {
                selection.nameOfNode            = parse_chunk_index_list(value, "nameOfNode");
                selection.name_of_node_explicit = true;
            }
            else if (key == "nodeOfName" || key == "node-name")
            {
                selection.nodeOfName            = parse_chunk_index_list(value, "nodeOfName");
                selection.node_of_name_explicit = true;
            }
            else if (key == "route-node" || key == "route_node")
            {
                selection.route_nodes          = parse_node_id_list(value, "route-node");
                selection.route_nodes_explicit = true;
            }
            else if (key == "route-name" || key == "route_name")
            {
                selection.route_name          = value;
                selection.route_name_explicit = true;
            }
            else if (key == "route-lang" || key == "route_lang")
            {
                selection.route_lang = value;
            }
            else if (key == "manifest")
            {
                use_manifest       = true;
                source_or_manifest = value;
            }
            else if (key == "source-bin" || key == "source_bin")
            {
                source_bin_override = value;
            }
            else if (key == "shard-root" || key == "shard_root")
            {
                shard_root = value;
            }
            else
            {
                throw std::runtime_error("Command .load-partial: Unknown selector '" + key + "'");
            }
        }

        if (use_manifest && source_bin_override.empty() && first_arg.ends_with(".bin"))
        {
            source_bin_override = first_arg;
        }

        if ((selection.route_nodes_explicit || selection.route_name_explicit) && !use_manifest)
        {
            throw std::runtime_error("Command .load-partial: route selectors require manifest mode");
        }

        if (selection.route_name_explicit && selection.route_lang.empty())
        {
            throw std::runtime_error("Command .load-partial: route-name requires route-lang=<lang>");
        }

        if (meta_only)
        {
            selection = {};
        }

        if (_repl_state->auto_run)
        {
            _repl_state->auto_run = false;
            _n->out("Auto-run has been disabled due to partial loading.", true);
        }

        chrono::StopWatch watch;
        watch.start();
        if (use_manifest)
        {
            _n->load_from_manifest(source_or_manifest, selection, shard_root, source_bin_override, meta_only);
        }
        else
        {
            _n->load_from_file(source_or_manifest, selection, meta_only);
        }
        watch.stop();

        _data_manager                    = nullptr;
        _repl_state->partial_load_mode   = true;
        _repl_state->partial_load_source = source_or_manifest;
        _n->out("WARNING: partial/incomplete graph loaded; reasoning, pruning, cleanup, and destructive edits are blocked.", true);
        _n->diagnostic(" Time needed for partial loading: " + watch.format(), true);
    }
    void cmd_wikidata_constraints(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 3) throw std::runtime_error("Command .wikidata-constraints: Missing json file name or directory name");
        if (cmd.size() > 3) throw std::runtime_error("Command .wikidata-constraints: Unknown argument after directory name");

        chrono::StopWatch watch;
        watch.start();

        const std::string&    dir        = cmd[2];
        std::filesystem::path input_path = cmd[1];

        // Specific Logic: This command strictly requires Wikidata capability.
        // We update the global manager to reflect this load context.
        _data_manager = io::DataManager::create(_n, input_path);

        // Dynamic cast to check if the factory returned a Wikidata manager
        auto wikidata_mgr = std::dynamic_pointer_cast<wikidata::Wikidata>(_data_manager);

        if (wikidata_mgr)
        {
            wikidata_mgr->import_all(dir);
        }
        else
        {
            // Fallback: If create() returned Generic (e.g. user pointed to a bin file without source),
            // but user wants constraints. This implies user error (missing source) or misuse.
            // But if user supplied JSON, create() definitely returns Wikidata.
            // If user supplied BIN, create() checks for source. If no source, it returns Generic.
            // If Generic, we can't export constraints.
            throw std::runtime_error("Cannot export constraints: Original Wikidata source file not found or invalid format.");
        }

        _n->diagnostic(" Time needed for exporting constraints: " + std::to_string(static_cast<double>(watch.duration()) / 1000) + "s", true);
    }
    void cmd_wikidata_qualifiers(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .wikidata-qualifiers: Missing json file name");

        if (_repl_state->partial_load_mode)
        {
            throw std::runtime_error("Command .wikidata-qualifiers: blocked while a partial/incomplete graph is loaded");
        }

        std::vector<std::string> qualifier_properties(cmd.begin() + 2, cmd.end());
        for (const auto& p : qualifier_properties)
        {
            bool valid = p.size() >= 2 && p[0] == 'P';
            for (size_t i = 1; valid && i < p.size(); ++i)
            {
                valid = p[i] >= '0' && p[i] <= '9';
            }
            if (!valid)
            {
                throw std::runtime_error("Command .wikidata-qualifiers: '" + p
                                         + "' is not a Wikidata property ID (expected P<number>)");
            }
        }

        if (_repl_state->auto_run)
        {
            _repl_state->auto_run = false;
            _n->out("Auto-run has been disabled due to importing a large dataset.", true);
        }

        chrono::StopWatch watch;
        watch.start();

        // Local manager on purpose: _data_manager stays associated with the
        // network loaded via .load.
        auto data_manager = io::DataManager::create(_n, cmd[1]);
        auto wikidata_mgr = std::dynamic_pointer_cast<wikidata::Wikidata>(data_manager);
        if (!wikidata_mgr)
        {
            throw std::runtime_error("Command .wikidata-qualifiers: '" + cmd[1]
                                     + "' is not a Wikidata JSON dump (.json or .json.bz2)");
        }

        wikidata_mgr->import_qualifiers(qualifier_properties);

        watch.stop();
        _n->diagnostic(" Time needed for qualifier import: " + watch.format(), true);
    }

    void cmd_export_wikidata(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 3)
            throw std::runtime_error("Usage: .export-wikidata <wikidata-dump.json> <Q...> [Q...]");

        const std::string&       json_file = cmd[1];
        std::vector<std::string> ids(cmd.begin() + 2, cmd.end());

        auto dm       = io::DataManager::create(_n, json_file);
        auto wikidata = std::dynamic_pointer_cast<wikidata::Wikidata>(dm);

        if (!wikidata)
            throw std::runtime_error("File is not recognized as Wikidata JSON (no matching .json/.json.bz2 found).");

        wikidata->export_entities(ids);
        _n->diagnostic("Export finished. *.json files are in the current directory.", true);
    }
#endif
    void cmd_list_rules(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .list-rules takes no arguments");

        // Get all nodes that are subjects of a core.Causes() relation
        network::adjacency_set rule_nodes = _n->get_rules();
        if (rule_nodes.empty())
        {
            _n->out("No rules found.", true);
            return;
        }

        _n->out("Listing all rules:", true);
        _n->out("------------------------", true);

        for (const auto& rule : rule_nodes)
        {
            std::string output;
            string::node_to_string(_n, output, _n->lang(), rule, 3);

            // node_to_string leaves the identifier markers in; every other
            // command strips them. Printing them made the listing the one
            // place where zelph shows its internals -- and the one place
            // where a rule could not be copied back in, because a
            // multi-word predicate came out as «is part of» rather than
            // quoted.
            _n->out(string::unmark_identifiers(output), true);
        }
        _n->out("------------------------", true);
    }
    void cmd_list_predicate_usage(const std::vector<std::string>& cmd)
    {
        size_t limit = 0;
        if (cmd.size() > 2) throw std::runtime_error("Command .list-predicate-usage accepts at most one optional argument (max entries)");
        // parse_count, not a hand-rolled stoull: the same negative-wraps-to-
        // huge trap applies here, and one place to get it right is enough.
        if (cmd.size() == 2) limit = string::parse_count(cmd[1]);
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_usage(limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }
    void cmd_list_predicate_value_usage(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .list-predicate-value-usage requires one required argument (<predicate>) and one optional (max entries)");

        size_t             limit    = 0;
        const std::string& pred_arg = cmd[1];
        if (cmd.size() == 3) limit = string::parse_count(cmd[2]);
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_value_usage(pred_arg, limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }

    void cmd_remove_rules(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .remove-rules takes no arguments");
        require_full_graph_mode(".remove-rules");
        _n->remove_rules();
        _n->out("All rules removed.", true);
    }
    void cmd_prune(const std::vector<std::string>& cmd, bool facts_mode)
    {
        require_full_graph_mode(facts_mode ? ".prune-facts" : ".prune-nodes");
        if (cmd.size() < 2)
            throw std::runtime_error("Command requires a pattern");

        // Reconstruct the pattern string from arguments to feed into the parser
        std::string pattern_str;
        for (size_t i = 1; i < cmd.size(); ++i)
        {
            const std::string& token = cmd[i];
            // If it's a variable, keep as is (A). If not, quote it ("is") so PEG treats it as value.
            // Note: Since cmd is already tokenized by escaped_list_separator, quotes were stripped.
            // We re-add them for non-vars to be safe for parse_zelph_to_janet.
            if (string::is_var(token))
                pattern_str += token + " ";
            else
                pattern_str += "\"" + token + "\" ";
        }

        std::string utf8_pattern = pattern_str;

        // Delegate parsing and evaluation to ScriptEngine
        std::string janet_code = _script_engine->parse_zelph_to_janet(utf8_pattern);

        if (janet_code.empty())
            throw std::runtime_error("Could not parse pattern");

        // Evaluating the pattern MATERIALIZES it, exactly as it does for
        // .explain -- and a REMOVAL command that adds what it was asked to
        // delete is the worst kind of surprise: ".prune-facts Q42 typo Q7"
        // used to insert that very fact. The construction therefore runs
        // inside a scratch cluster which is dropped afterwards, so a
        // pattern the graph did not already contain leaves no trace.
        static const std::string scratch  = "__prune";
        const std::string        previous = _n->active_cluster_name();
        _n->set_active_cluster(scratch);

        network::Node pattern_fact = 0;
        try
        {
            pattern_fact = _script_engine->evaluate_expression(janet_code, /*quiet*/ true);
        }
        catch (...)
        {
            restore_cluster(previous);
            _n->drop_cluster(scratch);
            throw;
        }

        const auto discard_pattern = [&]
        {
            restore_cluster(previous);
            _n->drop_cluster(scratch);
        };

        if (pattern_fact == 0)
        {
            discard_pattern();
            throw std::runtime_error("Invalid pattern");
        }

        // A pattern without variables denotes exactly ONE fact, and the
        // unification scan that finds "all facts matching the pattern" has
        // nothing to bind, so it found none -- ".prune-facts a rel b"
        // silently did nothing. Dropping the scratch first answers the only
        // question that remains: whatever survives existed beforehand.
        std::unordered_set<network::Node> pattern_vars;
        {
            std::vector<network::Node> history;
            network::collect_variables(_n, pattern_fact, pattern_vars, 1, history);
        }

        if (pattern_vars.empty())
        {
            discard_pattern();

            const bool exists = _n->check_fact(pattern_fact).is_known();
            if (exists) _n->remove_node(pattern_fact);

            const std::string what = exists ? "1" : "0";
            if (facts_mode)
                _n->out("Pruned " + what + " matching facts.", true);
            else
                _n->out("Pruned " + what + " matching facts and 0 nodes (a pattern without variables binds nothing to delete).", true);

            if (exists) _n->diagnostic("Consider running .cleanup.", true);
            return;
        }

        if (facts_mode)
        {
            size_t removed = 0;
            _n->prune_facts(pattern_fact, removed);
            discard_pattern();
            _n->out("Pruned " + std::to_string(removed) + " matching facts.", true);
            if (removed > 0) _n->diagnostic("Consider running .cleanup.", true);
        }
        else
        {
            network::Node relation = _n->parse_relation(pattern_fact);
            if (network::Network::is_var(relation))
            {
                discard_pattern();
                throw std::runtime_error("Command .prune-nodes: relation (predicate) must be fixed");
            }

            // One variable is the documented requirement, and it is the
            // only one the command can honour: with two, it deleted the
            // SUBJECT bindings and left the object ones alone, without
            // saying so. On a loaded dump that is half a deletion nobody
            // asked for.
            if (pattern_vars.size() > 1)
            {
                discard_pattern();
                throw std::runtime_error("Command .prune-nodes: exactly one variable is allowed (the subject or a single object) -- "
                                         "it names what gets deleted. Use .prune-facts to remove facts without deleting nodes.");
            }
            size_t removed_facts = 0;
            size_t removed_nodes = 0;
            _n->prune_nodes(pattern_fact, removed_facts, removed_nodes);
            discard_pattern();
            _n->out("Pruned " + std::to_string(removed_facts) + " matching facts and " + std::to_string(removed_nodes) + " nodes.", true);
            if (removed_facts > 0 || removed_nodes > 0)
            {
                _n->diagnostic("Consider running .cleanup.", true);
            }
        }
    }
    void cmd_cleanup(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".cleanup");
        if (cmd.size() != 1)
            throw std::runtime_error("Command .cleanup takes no arguments");

        size_t removed_facts = 0;
        size_t removed_preds = 0;

        _n->diagnostic("Scanning for unused predicates and zombie facts...", true);

        _n->purge_unused_predicates(removed_facts, removed_preds);

        _n->out("Purged " + std::to_string(removed_facts) + " zombie facts.", true);
        _n->out("Removed " + std::to_string(removed_preds) + " unused predicates.", true);

        _n->diagnostic("Cleaning up isolated nodes...", true);

        size_t cleanup_count = 0;
        _n->cleanup_isolated(cleanup_count);
        _n->out("Cleanup: removed " + std::to_string(cleanup_count) + " isolated nodes/names.", true);

        _n->diagnostic("Cleaning up name mappings...", true);
        size_t names_removed = _n->cleanup_names();
        _n->out("Removed " + std::to_string(names_removed) + " dangling name entries.", true);
    }
    void cmd_new(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .new takes no arguments");
        _repl_state->reset_requested = true;
    }
    void cmd_stat(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .stat takes no arguments");

        _n->out_stream() << "Network Statistics:" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        _n->out_stream() << "Nodes: " << _n->count() << std::endl;

        size_t ram_usage = zelph::platform::get_process_memory_usage();
        if (ram_usage > 0)
        {
            _n->out_stream() << "RAM Usage: " << std::fixed << std::setprecision(1)
                             << (static_cast<double>(ram_usage) / (1024 * 1024 * 1024)) << " GiB" << std::endl;
        }

        if (_n->language_count() > 0)
        {
            _n->out_stream() << "Name-of-Node Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                _n->out_stream() << "  " << lang << ": " << _n->get_name_of_node_size(lang) << std::endl;
            }

            _n->out_stream() << "Node-of-Name Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                _n->out_stream() << "  " << lang << ": " << _n->get_node_of_name_size(lang) << std::endl;
            }
        }

        _n->out_stream() << "Languages: " << _n->language_count() << std::endl;
        _n->out_stream() << "Rules: " << _n->rule_count() << std::endl;

        _n->out_stream() << "------------------------" << std::endl;
    }
#ifndef __EMSCRIPTEN__
    void cmd_stat_file(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .stat-file requires exactly one argument: the input .bin file");

        const std::string& filename     = cmd[1];
        BinHeaderStats     stats        = read_bin_header_stats(filename);
        uint64_t           total_chunks = static_cast<uint64_t>(stats.left_chunk_count)
                                        + static_cast<uint64_t>(stats.right_chunk_count)
                                        + static_cast<uint64_t>(stats.name_of_node_count)
                                        + static_cast<uint64_t>(stats.node_of_name_count);

        _n->out_stream() << "Serialized File Statistics:" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;
        _n->out_stream() << "File: " << filename << std::endl;
        _n->out_stream() << "File Size: " << stats.file_size_bytes << " bytes" << std::endl;
        _n->out_stream() << "Left Chunks: " << stats.left_chunk_count << std::endl;
        _n->out_stream() << "Right Chunks: " << stats.right_chunk_count << std::endl;
        _n->out_stream() << "Name-of-Node Chunks: " << stats.name_of_node_count << std::endl;
        _n->out_stream() << "Node-of-Name Chunks: " << stats.node_of_name_count << std::endl;
        _n->out_stream() << "Total Chunks: " << total_chunks << std::endl;
        _n->out_stream() << "------------------------" << std::endl;
    }
    void cmd_index_file(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 3) throw std::runtime_error("Command .index-file requires exactly two arguments: the input .bin file and output .json file");

        BinIndexData data = read_bin_index_data(cmd[1]);
        write_bin_index_json(data, cmd[2]);
        _n->out("Wrote byte-offset index to " + cmd[2], true);
    }
#endif
    void cmd_licenses(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .licenses takes no arguments");

        std::istringstream stream(zelph::get_version_description());
        std::string        line;

        while (std::getline(stream, line))
        {
            // Wir überspringen leere Zeilen am Ende nicht,
            // aber std::getline verwirft das '\n'.
            _n->out(line, true);
        }
    }
    void cmd_log(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .log: exactly one maximum recursion depth required (0 = off, -1 = only statistics).");

        int depth;
        try
        {
            depth = std::stoi(cmd[1]);
        }
        catch (...)
        {
            throw std::runtime_error("Command .log: invalid depth value.");
        }

        _n->set_logging(depth);
    }
    void cmd_log_janet(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .log-janet takes no arguments");

        _script_engine->toggle_janet_logging();
        _n->out("Janet function logging is now " + _script_engine->get_janet_logging_status() + ".", true);
    }
    void cmd_prof(const std::vector<std::string>& cmd)
    {
        if (cmd.size() > 2 || (cmd.size() == 2 && cmd[1] != "reset"))
            throw std::runtime_error("Usage: .prof [reset]");
        _n->profiler_dump(cmd.size() == 2);
    }
#ifndef __EMSCRIPTEN__
    void cmd_save(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".save");
        if (cmd.size() != 2)
            throw std::runtime_error("Command .save requires exactly one argument: the output file (must end with .bin)");

        const std::string& file = cmd[1];
        if (!file.ends_with(".bin"))
            throw std::runtime_error("Command .save: filename must end with '.bin'");

        _n->save_to_file(file);
        _n->diagnostic("Saved network to " + file, true);
    }

    void cmd_save_predicates(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".save-predicates");
        if (cmd.size() < 3)
            throw std::runtime_error("Command .save-predicates requires an output file (.bin) and at least one predicate");

        const std::string& file = cmd[1];
        if (!file.ends_with(".bin"))
            throw std::runtime_error("Command .save-predicates: filename must end with '.bin'");

        std::vector<network::Node> predicates;
        for (size_t i = 2; i < cmd.size(); ++i)
        {
            const network::Node nd = resolve_node(cmd[i], _n->lang());
            if (nd == 0)
                throw std::runtime_error("Command .save-predicates: unknown predicate '" + cmd[i]
                                         + "' in language '" + _n->lang() + "'");
            predicates.push_back(nd);
        }

        const size_t facts = _n->save_predicate_slice(file, predicates);
        _n->diagnostic("Saved " + std::to_string(facts) + " fact(s) of "
                           + std::to_string(predicates.size()) + " predicate(s) to " + file,
                       true);
    }
#endif
    void cmd_import(const std::vector<std::string>& cmd) const
    {
        require_full_graph_mode(".import");
        if (cmd.size() < 2) throw std::runtime_error("Command .import: Missing script path");
        // Tokens after the script path are passed to the script as arguments.
        import_file(cmd[1], std::vector<std::string>(cmd.begin() + 2, cmd.end()));
    }
    void cmd_provides(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .provides: missing module ID");

        // Inside an imported script this is effectively a no-op: import_file
        // pre-scans .provides lines and registers the IDs before execution
        // (attributed to the script's default ID). The command still needs a
        // handler so the line is not rejected -- and interactively it claims
        // an ID directly, which blocks all scripts providing that ID.
        for (size_t i = 1; i < cmd.size(); ++i)
        {
            const std::string id = string::to_lower_ascii(cmd[i]);
            _repl_state->imported_module_ids.emplace(id, id);
        }
    }
    void cmd_auto_run(const std::vector<std::string>& cmd)
    {
        // A toggle standing among .anchors/.semi-naive/.fact-stores, which
        // all take [on|off]. Silently ignoring an argument meant that
        // ".auto-run off" ENABLED auto-run whenever it happened to be off.
        if (cmd.size() != 1) throw std::runtime_error("Usage: .auto-run  (a toggle; it takes no argument)");
        _repl_state->auto_run = !_repl_state->auto_run;
        _n->out("Auto-run is now " + std::string(_repl_state->auto_run ? "enabled" : "disabled") + ".", true);
    }
    void cmd_deductions(const std::vector<std::string>& cmd)
    {
        if (cmd.size() > 2) throw std::runtime_error("Usage: .deductions [all|focus|off]");

        if (cmd.size() >= 2)
        {
            if (cmd[1] == "all")
            {
                _repl_state->deduction_mode = DeductionMode::All;
                _n->clear_input_focus();
            }
            else if (cmd[1] == "focus")
            {
                _repl_state->deduction_mode = DeductionMode::Focus;
                _n->clear_input_focus();
            }
            else if (cmd[1] == "off")
            {
                _repl_state->deduction_mode = DeductionMode::Off;
                _n->clear_input_focus();
            }
            else
            {
                throw std::runtime_error("Usage: .deductions [all|focus|off]");
            }
        }
        _n->set_deduction_filter(_repl_state->deduction_mode == DeductionMode::Focus);
        const char* name = _repl_state->deduction_mode == DeductionMode::All   ? "all"
                         : _repl_state->deduction_mode == DeductionMode::Focus ? "focus"
                                                                               : "off";
        _n->out("Deduction printing mode: " + std::string(name), true);
    }
    void cmd_parallel(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .parallel takes no arguments");

        _n->toggle_parallel();
        _n->out("Parallel processing is now " + std::string(_n->use_parallel() ? "enabled" : "disabled") + ".", true);
    }

    void cmd_anchors(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            _n->out(std::string("Anchor-based lookups: ") + (_n->use_anchors() ? "on" : "off"), true);
            return;
        }
        if (cmd.size() != 2 || (cmd[1] != "on" && cmd[1] != "off"))
            throw std::runtime_error("Usage: .anchors [on|off]");

        _n->set_anchors(cmd[1] == "on");
        _n->out(std::string("Anchor-based lookups: ") + (_n->use_anchors() ? "on" : "off"), true);
    }

    void cmd_semi_naive(const std::vector<std::string>& cmd)
    {
        auto status = [this]() -> std::string
        {
            if (!_n->seminaive()) return "off";
            return _n->seminaive_check() ? "check" : "on";
        };

        if (cmd.size() == 1)
        {
            _n->out("Semi-naive evaluation: " + status(), true);
            return;
        }

        if (cmd.size() != 2)
            throw std::runtime_error("Usage: .semi-naive [on|off|check]");

        if (cmd[1] == "on")
        {
            _n->set_seminaive(true);
            _n->set_seminaive_check(false);
        }
        else if (cmd[1] == "off")
        {
            _n->set_seminaive(false);
            _n->set_seminaive_check(false);
        }
        else if (cmd[1] == "check")
        {
            _n->set_seminaive(true);
            _n->set_seminaive_check(true);
        }
        else
        {
            throw std::runtime_error("Usage: .semi-naive [on|off|check]");
        }

        _n->out("Semi-naive evaluation: " + status(), true);
    }

    void cmd_fact_stores(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            _n->out(std::string("Fact-path stores: ") + (_n->fact_stores_enabled() ? "on" : "off"), true);
            return;
        }
        if (cmd.size() != 2 || (cmd[1] != "on" && cmd[1] != "off"))
            throw std::runtime_error("Usage: .fact-stores [on|off]");

        if (cmd[1] == "off")
        {
            _n->disable_fact_stores();
            _n->out("Fact-path stores: off", true);
            return;
        }

        if (_n->fact_stores_enabled())
            _n->out("Fact-path stores: on", true);
        else
            throw std::runtime_error(".fact-stores on: the stores cannot be re-armed once disabled, "
                                     "because absence of an entry is meaningful while a store is "
                                     "authoritative. Use .new to start with a fresh engine and stores enabled again.");
    }

    void cmd_cluster(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            const std::string active = _n->active_cluster_name();
            _n->out("Active cluster: " + (active.empty() ? "default" : active), true);
            for (const auto& [name, count] : _n->list_clusters())
                _n->out("  " + name + ": " + std::to_string(count) + " node(s)", true);
            return;
        }
        if (cmd.size() != 2) throw std::runtime_error("Usage: .cluster [name]");

        if (cmd[1] == "default")
        {
            _n->deactivate_cluster();
            _n->out("Active cluster: default", true);
        }
        else
        {
            _n->set_active_cluster(cmd[1]);
            _n->out("Active cluster: " + cmd[1], true);
        }
    }

    void cmd_cluster_drop(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Usage: .cluster-drop <name>");
        if (cmd[1] == "default") throw std::runtime_error(".cluster-drop: the default cluster cannot be dropped");

        // An unknown name is an error, exactly as in .cluster-merge -- and
        // it has to be checked here, because "removed 0 node(s)" is also the
        // honest report for a cluster that exists but is empty. Reporting a
        // typo as a successful rollback is how an experiment silently keeps
        // running against a cluster the user believes is gone.
        const auto clusters = _n->list_clusters();
        if (std::none_of(clusters.begin(), clusters.end(), [&](const auto& c)
                         { return c.first == cmd[1]; }))
            throw std::runtime_error(".cluster-drop: unknown cluster '" + cmd[1] + "'");

        const bool   was_active = _n->active_cluster_name() == cmd[1];
        const size_t removed    = _n->drop_cluster(cmd[1]);
        _n->out("Dropped cluster " + cmd[1] + ": removed " + std::to_string(removed) + " node(s).", true);
        if (was_active) _n->out("Active cluster: default", true);
    }

    void cmd_cluster_merge(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 3) throw std::runtime_error("Usage: .cluster-merge <from> <to>  (to may be 'default')");
        const std::string to = (cmd[2] == "default") ? "" : cmd[2];
        if (!_n->merge_cluster(cmd[1], to))
            throw std::runtime_error(".cluster-merge: unknown cluster '" + cmd[1] + "'");
        _n->out("Merged cluster " + cmd[1] + " into " + cmd[2] + ".", true);
    }

    // parse_zelph_to_janet for a fact pattern: an unparsable pattern is a
    // normal outcome here (see cmd_explain's unwrapping), not an error to
    // propagate.
    std::string try_parse_pattern(const std::string& pattern) const
    {
        try
        {
            return _script_engine->parse_zelph_to_janet(pattern);
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    void restore_cluster(const std::string& name) const
    {
        if (name.empty() || name == "default")
            _n->deactivate_cluster();
        else
            _n->set_active_cluster(name);
    }

    // Evaluating a statement MATERIALIZES it (the zelph AST calls
    // zelph/fact), which would make every pattern "asserted" and turn
    // .explain into an assertion command. The evaluation therefore runs
    // inside a scratch cluster that is rolled back immediately: nodes that
    // already existed are never recorded, so the drop removes exactly what
    // this evaluation added -- and nothing else. The returned node ID is a
    // structural hash and stays meaningful after the rollback, so
    // check_fact() can answer honestly.
    network::Node evaluate_pattern_read_only(const std::string& code)
    {
        static const std::string scratch  = "__explain";
        const std::string        previous = _n->active_cluster_name();

        _n->set_active_cluster(scratch);

        network::Node target = 0;
        try
        {
            target = _script_engine->evaluate_expression(code, /*quiet*/ true);
        }
        catch (...)
        {
            restore_cluster(previous);
            _n->drop_cluster(scratch);
            throw;
        }

        restore_cluster(previous);
        _n->drop_cluster(scratch);
        return target;
    }

    // Tokens -> the node the pattern denotes, or 0 if this reading does not
    // work out. Every failure mode is folded into 0 so that cmd_explain can
    // TRY a reading: parse failure, a statement the AST builder rejects
    // (too few components), and a pattern that denotes nothing.
    network::Node resolve_explain_pattern(const std::vector<std::string>& parts)
    {
        if (parts.empty()) return 0;

        // tokenize_quoted has already STRIPPED the quotes, so a token that
        // still contains whitespace can only have come from a quoted one --
        // ".explain a \"is not\" b" arrives as {"a", "is not", "b"}. Joining
        // that with blanks hands the parser a four-component statement and
        // resolves the wrong node, so the fact zelph printed as `a "is not"
        // b` could not be explained by pasting it back. Re-quote exactly
        // those tokens; everything else (nested facts, term islands, ¬,
        // &-literals) has to stay verbatim to keep parsing.
        std::string pattern;
        for (const auto& p : parts)
        {
            if (!pattern.empty()) pattern += ' ';
            if (p.find_first_of(" \t") == std::string::npos)
                pattern += p;
            else
                pattern += '"' + p + '"'; // the PEG's quoted atom has no
                                          // escapes, so a name containing a
                                          // quote fails to parse -- honestly,
                                          // rather than resolving something else
        }

        // A pattern wrapped in a single pair of parentheses --
        // ".explain ((&6 + &7) = &13)" -- is a TERM, which the statement
        // grammar rejects; unwrapping yields the statement the user
        // meant. Tried SECOND, so a pattern that parses as given keeps
        // its original reading.
        std::string code = try_parse_pattern(pattern);
        if (code.empty() && is_fully_parenthesized(pattern))
            code = try_parse_pattern(pattern.substr(1, pattern.size() - 2));
        if (code.empty()) return 0;

        try
        {
            return evaluate_pattern_read_only(code);
        }
        catch (const std::exception&)
        {
            return 0;
        }
    }

    void cmd_explain(const std::vector<std::string>& cmd)
    {
        std::vector<std::string> parts(cmd.begin() + 1, cmd.end());

        const auto is_number = [](const std::string& s)
        { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };

        // Two readings of a trailing all-digit token, tried in this order:
        //
        //   (1) it is the max-depth argument -- the documented form,
        //       ".explain alice likes bob 5";
        //   (2) it belongs to the pattern -- which is the case whenever the
        //       fact's own object is a numeral, as in
        //       ".explain ((1 d+ 1) tci 0) sum 0". Reading (1) would steal
        //       the object there and leave a two-component statement that
        //       the AST builder cannot turn into a fact.
        //
        // (1) keeps precedence, so the documented form never changes
        // meaning; (2) only rescues arguments that (1) cannot resolve.
        std::size_t   depth  = 4;
        network::Node target = 0;

        if (!parts.empty() && is_number(parts.back()))
        {
            const std::vector<std::string> head(parts.begin(), parts.end() - 1);
            if (head.empty())
            {
                // Depth only: ".explain 3" explains the last output node.
                depth = std::stoul(parts.back());
                parts.clear();
            }
            else if ((target = resolve_explain_pattern(head)) != 0)
            {
                depth = std::stoul(parts.back());
            }
        }

        if (target == 0) target = resolve_explain_pattern(parts);

        if (target == 0)
        {
            if (!parts.empty())
                throw std::runtime_error(".explain: cannot parse fact pattern, or it does not denote a fact");

            target = string::last_node_to_string_node();
            if (!target)
                throw std::runtime_error(".explain: no previous output node -- pass a fact pattern");
        }

        if (!_n->check_fact(target).is_known())
        {
            _n->out("Fact is not asserted -- nothing to explain.", true);
            return;
        }

        std::set<network::Node> printed;
        std::string             out;
        render_proof(_n->explain(target, depth), "", true, printed, out);
        _n->out(out, true);
    }

    // Indented proof tree in the established "⇐" notation. Shared subproofs
    // (the DAG from hash-consing) are expanded once and referenced afterwards.
    void render_proof(const std::shared_ptr<network::ProofNode>& p, const std::string& indent, const bool last, std::set<network::Node>& printed, std::string& out) const
    {
        std::string line;
        zelph::string::node_to_string(_n, line, _n->lang(), p->fact, 3);
        line = zelph::string::unmark_identifiers(line);

        const std::string branch       = indent.empty() ? "" : indent + (last ? "└─ " : "├─ ");
        const std::string child_indent = indent.empty() ? "   " : indent + (last ? "   " : "│  ");

        switch (p->status)
        {
        case network::ProofNode::Status::Axiom:
            out += branch + line + "  [axiom]\n";
            return;
        case network::ProofNode::Status::Unfounded:
            out += branch + line + "  [no acyclic justification found in the current graph]\n";
            return;
        case network::ProofNode::Status::Truncated:
            out += branch + line + "  … [depth limit -- use '.explain <pattern> 0' for the full proof]\n";
            return;
        case network::ProofNode::Status::Derived:
            break;
        }

        if (printed.count(p->fact))
        {
            out += branch + line + "  [see above]\n";
            return;
        }
        printed.insert(p->fact);

        out += branch + line + "\n";

        const std::size_t total = p->premises.size() + p->absent.size();
        std::size_t       index = 0;
        for (const auto& premise : p->premises)
            render_proof(premise, child_indent, ++index == total, printed, out);
        for (const network::Node neg : p->absent)
        {
            // The stored node is the rule's negation-tagged pattern, so
            // node_to_string writes the ¬(...) itself; passing the step's
            // bindings turns "¬(N hasdivisor D)" into the premise actually
            // checked, "¬(&7 hasdivisor D)". D stays a variable on purpose --
            // it is what "for no D" quantifies over.
            std::string nline;
            zelph::string::node_to_string(_n, nline, _n->lang(), neg, 3, p->bindings);
            nline = zelph::string::unmark_identifiers(nline);
            if (nline.rfind("¬", 0) != 0) nline = "¬(" + nline + ")";
            out += child_indent + (++index == total ? "└─ " : "├─ ") + nline + "  [absent]\n";
        }
    }
};

console::CommandExecutor::CommandExecutor(network::Reasoning*        reasoning,
                                          ScriptEngine*              script_engine,
                                          std::shared_ptr<ReplState> repl_state,
                                          LineProcessor              line_processor)
    : _pImpl(new Impl(reasoning, script_engine, repl_state, std::move(line_processor)))
{
}

console::CommandExecutor::~CommandExecutor() = default;

void console::CommandExecutor::execute(const std::vector<std::string>& cmd)
{
    _pImpl->execute(cmd);
}

void console::CommandExecutor::finish_input()
{
    _pImpl->finish_input();
}

void console::CommandExecutor::import_file(const std::string& file, const std::vector<std::string>& args) const
{
    _pImpl->import_file(file, args);
}
