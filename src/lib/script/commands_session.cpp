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

#include "script/command_executor_impl.hpp"

#include "io/bin_inspect.hpp"
#include "io/data_manager.hpp"
#include "network/reasoning.hpp"
#include "platform/platform_utils.hpp"
#include "repl_state.hpp"
#include "script/script_engine.hpp"
#include "string/string_utils.hpp"
#include "versions.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

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

namespace zelph::console
{
#ifndef __EMSCRIPTEN__
    std::vector<uint32_t> CommandExecutor::Impl::parse_chunk_index_list(const std::string& value, const std::string& label)
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
#endif

#ifndef __EMSCRIPTEN__
    std::vector<uint64_t> CommandExecutor::Impl::parse_node_id_list(const std::string& value, const std::string& label)
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

    void CommandExecutor::Impl::import_file(const std::string& file, const std::vector<std::string>& args) const
    {
        // Resolve against the working directory first, then the standard
        // library ('.zph' extension optional). This also covers scripts
        // passed on the command line (Interactive::process_file ends up
        // here), so `zelph examples/english` works like `.import`.
        const std::string resolved = resolve_script_path(file);

        // A partial view is incomplete, so a script that ADDS to it deserves a
        // word -- but most scripts only define things. Which of the two it is
        // shows in the node count, and that is also the honest measure: it
        // counts what actually reached the graph, not what the file looks
        // like. Blocking .import outright instead made the layered query
        // languages unusable on a partial view (`.import sparql` after
        // `.load-partial`), while protecting nothing: every operation that
        // would be wrong on an incomplete graph -- inference, pruning,
        // cleanup, renaming, saving -- is refused on its own, from Janet as
        // well, and a plain typed statement was always allowed.
#ifndef __EMSCRIPTEN__
        const bool warn_about_writes = _repl_state && _repl_state->partial_load_mode
                                    && _repl_state->import_depth == 0;
#else
        const bool warn_about_writes = false; // no partial loading in the wasm build
#endif
        const network::Node nodes_before = warn_about_writes ? _n->count() : 0;

        std::vector<std::string> module_ids;

        // Import guard for .zph scripts (Janet scripts are runnable programs
        // that may legitimately be executed repeatedly, e.g. with different
        // arguments -- they are exempt).
        if (std::filesystem::path(resolved).extension() != ".janet")
        {
            module_ids              = scan_module_ids(resolved);
            const std::string& self = module_ids.front(); // default ID

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

        // ... and release the claim again unless the import runs to
        // completion. A script that threw halfway used to keep its IDs, so
        // importing it again after the fix reported "Skipping already
        // imported" and did nothing: the only way out was .new. What is left
        // behind now is what the lines before the error did, and the next
        // attempt runs.
        struct ModuleIdClaim
        {
            ReplState&                      state;
            const std::vector<std::string>& ids;
            bool                            committed = false;

            ~ModuleIdClaim()
            {
                if (committed) return;
                for (const auto& id : ids)
                    state.imported_module_ids.erase(id);
            }
        } module_id_claim{*_repl_state, module_ids};

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

        module_id_claim.committed = true;

        if (warn_about_writes && _n->count() > nodes_before)
        {
            _n->error("WARNING: '" + file + "' added " + std::to_string(_n->count() - nodes_before)
                          + " node(s) to a partial view.\n"
                            "  Inference over them is blocked (.run), and the adjacency-index cache is\n"
                            "  disabled for this session because the graph no longer matches its file.",
                      true);
        }

        if (suspend.was_active())
        {
            _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false, true);
        }
    }

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_load(const std::vector<std::string>& cmd)
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
#endif

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_load_partial(const std::vector<std::string>& cmd)
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
#endif

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_stat_file(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .stat-file requires exactly one argument: the input .bin file");

        const std::string& filename     = cmd[1];
        io::BinHeaderStats stats        = io::read_bin_header_stats(".stat-file", filename);
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
        // The counts come from the header; nothing here read a chunk. That is
        // the point on an 88 GB file, but it also means a file that stops
        // after the header still reports them. .index-file walks the chunks
        // and is what fails on such a file.
        _n->out_stream() << "(declared by the header; use .index-file to verify the chunks)" << std::endl;
    }
#endif

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_index_file(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 3) throw std::runtime_error("Command .index-file requires exactly two arguments: the input .bin file and output .json file");

        io::BinIndexData data = io::read_bin_index_data(".index-file", cmd[1]);
        io::write_bin_index_json(data, cmd[2]);
        _n->out("Wrote byte-offset index to " + cmd[2], true);
    }
#endif

    void CommandExecutor::Impl::cmd_licenses(const std::vector<std::string>& cmd)
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

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_save(const std::vector<std::string>& cmd)
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
#endif

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_save_predicates(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".save-predicates");
        if (cmd.size() < 3)
            throw std::runtime_error("Command .save-predicates requires an output file (.bin) and at least one predicate");

        const std::string& file = cmd[1];
        if (!file.ends_with(".bin"))
            throw std::runtime_error("Command .save-predicates: filename must end with '.bin'");

        // A predicate is a NODE, and a fact is a node, so a fact can be a
        // predicate -- `.list-predicate-usage` lists `a p b` among the
        // predicates and `S (a p b) O` answers its facts. Naming one here has
        // to work too, or the listing shows a slice you cannot cut. The
        // parenthesised form is what the renderer prints and what `.explain`,
        // `.node` and the prune commands already take; the tokenizer splits it
        // on whitespace, so the run from "(" to its closing ")" is regrouped
        // and resolved as one pattern.
        std::vector<network::Node> predicates;
        for (size_t i = 2; i < cmd.size(); ++i)
        {
            if (!cmd[i].empty() && cmd[i].front() == '(')
            {
                int    depth = 0;
                size_t end   = i;
                for (; end < cmd.size(); ++end)
                {
                    for (const char c : cmd[end])
                    {
                        if (c == '(')
                            ++depth;
                        else if (c == ')')
                            --depth;
                    }
                    if (depth <= 0) break;
                }

                if (depth != 0 || end >= cmd.size())
                    throw std::runtime_error("Command .save-predicates: unbalanced parentheses in the predicate pattern");

                const std::vector<std::string> group(cmd.begin() + static_cast<long>(i), cmd.begin() + static_cast<long>(end) + 1);

                // The offset has to travel with the group. pattern_code reads
                // the ORIGINAL tokens out of _sources to recover what the
                // tokenizer quoted, indexing them by `first + i` -- and
                // resolve_node_or_fact hardcodes first = 1, which is right for
                // a command whose pattern starts immediately after its name
                // and wrong here, where the file path stands between them. It
                // built a pattern out of the path and the first two tokens.
                const network::Node nd = resolve_explain_pattern(group, i);
                if (nd == 0 || !_n->exists(nd))
                {
                    std::string shown;
                    for (const auto& token : group)
                        shown += (shown.empty() ? "" : " ") + token;
                    throw std::runtime_error("Command .save-predicates: '" + shown
                                             + "' denotes no node of this network");
                }
                predicates.push_back(nd);
                i = end;
                continue;
            }

            const network::Node nd = resolve_node(cmd[i], _n->lang());
            if (nd == 0)
                throw std::runtime_error("Command .save-predicates: unknown predicate '" + cmd[i]
                                         + "' in language '" + _n->lang() + "'");
            predicates.push_back(nd);
        }

        size_t       rules = 0;
        const size_t facts = _n->save_predicate_slice(file, predicates, &rules);

        std::string message = "Saved " + std::to_string(facts) + " fact(s) of "
                            + std::to_string(predicates.size()) + " predicate(s)";
        if (rules != 0) message += " and " + std::to_string(rules) + " rule(s)";
        message += " to " + file;
        _n->diagnostic(message, true);
    }
#endif

    void CommandExecutor::Impl::cmd_import(const std::vector<std::string>& cmd) const
    {
        // Deliberately NOT gated by require_full_graph_mode: see import_file.
        if (cmd.size() < 2) throw std::runtime_error("Command .import: Missing script path");
        // Tokens after the script path are passed to the script as arguments.
        import_file(cmd[1], std::vector<std::string>(cmd.begin() + 2, cmd.end()));
    }

    void CommandExecutor::Impl::cmd_provides(const std::vector<std::string>& cmd)
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
}
