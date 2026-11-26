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

namespace
{
    using zelph::console::CommandExecutor;

    // The setting commands that a default of the process may be
    // (`zelph --default`), each with what it is a setting of. A toggle stands
    // alone, and its line switches the setting over from where a fresh engine
    // or session has it; every other command needs the value it sets, as
    // without one it merely displays the current setting. The three settings
    // of the session are precisely those that .new keeps.
    struct DefaultSetting
    {
        const char*                   command;
        bool                          toggle;
        CommandExecutor::DefaultScope scope;
    };

    constexpr DefaultSetting default_settings[] = {
        {".lang", false, CommandExecutor::DefaultScope::Engine},
        {".parallel", true, CommandExecutor::DefaultScope::Engine},
        {".threads", false, CommandExecutor::DefaultScope::Engine},
        {".anchors", false, CommandExecutor::DefaultScope::Engine},
        {".semi-naive", false, CommandExecutor::DefaultScope::Engine},
        {".fact-stores", false, CommandExecutor::DefaultScope::Engine},
        {".contradiction-records", false, CommandExecutor::DefaultScope::Engine},
        {".negation-records", false, CommandExecutor::DefaultScope::Engine},
        {".explain-budget", false, CommandExecutor::DefaultScope::Engine},
        {".node-map-load-factor", false, CommandExecutor::DefaultScope::Engine},
        {".log", false, CommandExecutor::DefaultScope::Engine},
        {".log-janet", true, CommandExecutor::DefaultScope::Engine},
        {".auto-run", true, CommandExecutor::DefaultScope::Session},
        {".deductions", false, CommandExecutor::DefaultScope::Session},
        {".timer", false, CommandExecutor::DefaultScope::Session},
    };
}

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
        // A block dispatched here serves as input, much as a block concluded
        // by a blank line or a closing `%` does, and Interactive::process
        // performs inference following such a block. Without that run, a
        // keyword block or a `%` block that was finalized by the end of
        // input derived nothing: a piped session printed `asserted` and
        // terminated, whereas the identical session with a trailing blank
        // line printed the derivation.
        _n->begin_input_capture();
        bool dispatched = false;
        // Whether the keyword handler was the final writer and ran the
        // engine afterwards, which skips the run below, as
        // Interactive::process skips it after a block that a blank line
        // dispatches. Any other input dispatched here is still followed by
        // the run.
        bool handler_ran_last = false;

        if (_repl_state->accumulating_keyword)
        {
            std::string keyword               = _repl_state->active_keyword;
            std::string text                  = _repl_state->keyword_buffer;
            _repl_state->accumulating_keyword = false;
            _repl_state->active_keyword.clear();
            _repl_state->keyword_buffer.clear();
            _repl_state->keyword_prev_blank = false;

            // Named as Interactive::process names a block that a blank line
            // dispatches; the end of input does not possess a line of its
            // own.
            const size_t runs_before = _n->completed_runs();
            try
            {
                dispatched = _script_engine->invoke_keyword(keyword, text, /*force*/ true);
            }
            catch (const std::exception& ex)
            {
                throw std::runtime_error("Error in line \"" + keyword_block_label(keyword, text) + "\": " + ex.what());
            }
            handler_ran_last = dispatched && _n->completed_runs() != runs_before && _n->unchanged_since_last_run();
        }

        if (_repl_state->accumulating_zelph && !_repl_state->zelph_buffer.empty())
        {
            const std::string buffered = string::strip_comments(_repl_state->zelph_buffer);
            _repl_state->zelph_buffer.clear();
            _repl_state->accumulating_zelph = false;

            // The accumulator has already decided this is not a statement
            // yet, and there are no further lines -- so say that, whether or
            // not the PEG can make something of the fragment. It can, for the
            // commonest typo there is: "a p" parses into a two-argument
            // zelph/fact and the user was handed Janet's own complaint,
            // "arity mismatch, expected at least 3, got 2", naming an
            // internal call they never made.
            const std::string transformed = _script_engine->is_zelph_complete(buffered)
                                              ? _script_engine->parse_zelph_to_janet(buffered)
                                              : std::string{};

            if (transformed.empty())
            {
                // Still incomplete with no more lines coming. Silence here
                // meant a truncated script or paste looked like it had run.
                throw std::runtime_error("Input ends inside an unfinished statement: " + string::trim(buffered));
            }
            handler_ran_last = false;
            _script_engine->process_janet(transformed, true);
            dispatched = true;
        }
        _repl_state->accumulating_zelph = false;

        // The state is left BEFORE the code executes. Left after it, Janet
        // code that threw stayed open: following a module that concluded
        // within a `%(` expression or a `%` block, the session's next lines
        // were appended to the module's code.
        const std::string janet_code = std::move(_repl_state->janet_buffer);
        _repl_state->janet_buffer.clear();
        _repl_state->accumulating_inline_janet = false;
        _repl_state->script_mode               = ScriptMode::Zelph;
        _script_engine->drop_inline_janet();

        if (!janet_code.empty())
        {
            handler_ran_last = false;
            _script_engine->process_janet(janet_code, false);
            dispatched = true;
        }

        // Within a module, auto_run is off, and the module's
        // single postponed run encompasses the dispatch made here.
        if (dispatched && _repl_state->auto_run && !handler_ran_last)
            _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false, true);
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

    CommandExecutor::DefaultScope CommandExecutor::Impl::apply_default(const std::vector<std::string>& cmd)
    {
        const DefaultSetting* setting = nullptr;
        if (!cmd.empty())
        {
            for (const DefaultSetting& candidate : default_settings)
                if (cmd[0] == candidate.command) setting = &candidate;
        }

        if (setting == nullptr)
        {
            std::string settings;
            for (const DefaultSetting& candidate : default_settings)
                settings += std::string(settings.empty() ? "" : ", ") + candidate.command;
            throw std::runtime_error("a default is one of the settings " + settings);
        }

        // Without its value, such a command shows the setting, which a
        // line producing no output would accomplish for nobody.
        if (!setting->toggle && cmd.size() == 1)
            throw std::runtime_error(std::string(setting->command) + " needs the value it is to set");

        // A setting command's output responds to the person who entered it, and
        // since no default was typed, only the error reports issued by the
        // command itself reach the output. The handler and the flag are
        // restored regardless of how the command ends.
        struct DefaultRun
        {
            network::Reasoning* n;
            io::OutputHandler   original;
            bool&               applying;

            DefaultRun(network::Reasoning* reasoning, bool& flag)
                : n(reasoning)
                , original(reasoning->get_output_handler())
                , applying(flag)
            {
                n->set_output_handler(
                    [handler = original](const io::OutputEvent& event)
                    {
                        if (handler && event.channel == io::OutputChannel::Error) handler(event);
                    });
                applying = true;
            }
            ~DefaultRun()
            {
                n->set_output_handler(std::move(original));
                applying = false;
            }
        } run(_n, _applying_defaults);

        execute(cmd, {});
        return setting->scope;
    }

    // --- Registration ---
    void CommandExecutor::Impl::register_commands()
    {
        _command_map[".help"] = [this](auto& c)
        { cmd_help(c); };
        // A `.quit` encountered in its bare form by the REPL loop within main()
        // never gets here: main() captures it before process(), enabling it to
        // function even within a keyword block. This handler deals with the
        // remaining cases -- `.quit now`, a `.quit` embedded within a session
        // script, one present in a module, or even in a module loaded via
        // `zelph/import` from Janet -- and merely records the request, deferring
        // the decision to the loop responsible for the input: main() respects it
        // after process() via Interactive::quit_requested(), the session-script
        // loop does so after processing each line, and the module-reading loop
        // clears the flag. Refer to ReplState::quit_requested.
        _command_map[".quit"] = [this](auto&)
        { _repl_state->quit_requested = true; };
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
        _command_map[".strata"] = [this](auto& c)
        { cmd_strata(c); };
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
        _command_map[".timer"] = [this](auto& c)
        { cmd_timer(c); };
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
        _command_map[".identity"] = [this](auto& c)
        { cmd_identity(c); };
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
        _command_map[".threads"] = [this](auto& c)
        { cmd_threads(c); };
        _command_map[".anchors"] = [this](auto& c)
        { cmd_anchors(c); };
        _command_map[".semi-naive"] = [this](auto& c)
        { cmd_semi_naive(c); };
        _command_map[".fact-stores"] = [this](auto& c)
        { cmd_fact_stores(c); };
        _command_map[".contradiction-records"] = [this](auto& c)
        { cmd_contradiction_records(c); };
        _command_map[".negation-records"] = [this](auto& c)
        { cmd_negation_records(c); };
        _command_map[".explain-budget"] = [this](auto& c)
        { cmd_explain_budget(c); };
        _command_map[".node-map-load-factor"] = [this](auto& c)
        { cmd_node_map_load_factor(c); };
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

console::CommandExecutor::DefaultScope console::CommandExecutor::apply_default(const std::vector<std::string>& cmd)
{
    return _pImpl->apply_default(cmd);
}

void console::CommandExecutor::require_full_graph_mode(const std::string& name) const
{
    _pImpl->require_full_graph_mode(name.c_str());
}

std::size_t console::CommandExecutor::import_file(const std::string& file, const std::vector<std::string>& args, const ScriptRole role, const std::size_t lines_done) const
{
    return _pImpl->import_file(file, args, role, lines_done);
}
