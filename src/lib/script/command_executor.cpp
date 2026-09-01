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

#include "script/command_executor.hpp"

#include "network/reasoning.hpp"
#include "repl_state.hpp"
#include "script/command_executor_impl.hpp"
#include "script/script_engine.hpp"
#include "string/string_utils.hpp"

#ifndef __EMSCRIPTEN__
    #include "wikidata/wikidata.hpp"

    #include "zelph.capnp.h"

    #include <capnp/message.h>
    #include <capnp/serialize-packed.h>
    #include <kj/io.h>
#endif

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace zelph;

// Alternative spellings of a command. One table drives BOTH the dispatch
// registration and ".help <alias>", so an alias can never exist as a
// runnable command while ".help" claims not to know it.
static const std::map<std::string, std::string> command_aliases = {
    {".why", ".explain"},
};

namespace zelph::console
{
    CommandExecutor::Impl::Impl(network::Reasoning* n, ScriptEngine* se, std::shared_ptr<ReplState> rs, CommandExecutor::LineProcessor lp) : _n(n), _script_engine(se), _repl_state(std::move(rs)), _process_line_callback(std::move(lp))
    {
        register_commands();
    }

    // Everything the line reader may still be holding on to. Both callers
    // reach the same situation from different directions: a script's last
    // line, and end of input in the REPL. The keyword handler's
    // :incomplete veto does not apply any more -- there are no further
    // lines to wait for -- so dispatch is forced, which turns an
    // unterminated block into an error instead of silence.
    void CommandExecutor::Impl::finish_input() const
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
            const std::string buffered = _repl_state->zelph_buffer;
            _repl_state->zelph_buffer.clear();
            _repl_state->accumulating_zelph = false;

            // The accumulator has already decided this is not a statement
            // yet, and there are no further lines -- so say that, whether or
            // not the PEG can make something of the fragment. It can, for the
            // commonest typo there is: "a p" parses into a two-argument
            // zelph/fact and the user was handed Janet's own complaint,
            // "arity mismatch, expected at least 3, got 2", naming an
            // internal call they never made.
            const std::string transformed = ScriptEngine::is_zelph_complete(buffered)
                                              ? _script_engine->parse_zelph_to_janet(buffered)
                                              : std::string{};

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

    // `sources` is each token in the form the PARSER needs, with the
    // quotes the tokenizer stripped put back. Only the commands that hand a
    // token back to the parser need it; everything else works on the plain
    // text. It is kept for the duration of THIS command and restored
    // afterwards, because a command can run a script whose lines are
    // commands again.
    void CommandExecutor::Impl::execute(const std::vector<std::string>& cmd, const std::vector<std::string>& sources)
    {
        if (cmd.empty()) return;

        auto it = _command_map.find(cmd[0]);
        if (it == _command_map.end())
        {
            throw std::runtime_error("Unknown command " + cmd[0] + ". Type .help for a list.");
        }

        struct SourceScope
        {
            std::vector<std::string>& slot;
            std::vector<std::string>  previous;

            SourceScope(std::vector<std::string>& s, const std::vector<std::string>& next)
                : slot(s)
                , previous(std::move(s)) { slot = next; }
            ~SourceScope() { slot = std::move(previous); }
        } scope(_sources, sources);

        it->second(cmd);
    }

    // --- Registration ---
    void CommandExecutor::Impl::register_commands()
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
        _command_map[".contradiction-records"] = [this](auto& c)
        { cmd_contradiction_records(c); };
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

    void CommandExecutor::Impl::require_full_graph_mode(const char* command_name) const
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
}

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
    _pImpl->execute(cmd, {});
}

void console::CommandExecutor::execute(const std::vector<std::string>& cmd, const std::vector<std::string>& sources)
{
    _pImpl->execute(cmd, sources);
}

void console::CommandExecutor::finish_input()
{
    _pImpl->finish_input();
}

void console::CommandExecutor::import_file(const std::string& file, const std::vector<std::string>& args) const
{
    _pImpl->import_file(file, args);
}
