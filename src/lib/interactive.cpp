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

#include "interactive.hpp"

#include "command_executor.hpp"
#include "reasoning.hpp"
#include "repl_state.hpp"
#include "script_engine.hpp"
#include "string_utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>

#include <memory>

#ifdef _WIN32
    #include <fcntl.h> // for _O_U16TEXT
    #include <io.h>    // for _setmode
    #include <stdio.h> // for _fileno
#endif

using namespace zelph;
using boost::escaped_list_separator;
using boost::tokenizer;

class console::Interactive::Impl
{
public:
    explicit Impl(Interactive* enclosing, OutputHandler output)
        : _n(new network::Reasoning(output))
        , _interactive(enclosing)
        , _script_engine(new ScriptEngine(_n))
        , _repl_state(std::make_shared<ReplState>())
    {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_U16TEXT);
#endif

        _n->set_lang("zelph");

        _n->register_core_node(_n->core.RelationTypeCategory, L"->");
        _n->register_core_node(_n->core.Causes, L"=>");
        _n->register_core_node(_n->core.IsA, L"~");
        _n->register_core_node(_n->core.Unequal, L"!=");
        _n->register_core_node(_n->core.Contradiction, L"!");
        _n->register_core_node(_n->core.Cons, L"cons");
        _n->register_core_node(_n->core.Nil, L"nil");
        _n->register_core_node(_n->core.PartOf, L"in");
        _n->register_core_node(_n->core.Conjunction, L"conjunction");
        _n->register_core_node(_n->core.Negation, L"negation");

        _script_engine->initialize();

        // Initialize CommandExecutor with references to our state
        _command_executor = std::make_unique<CommandExecutor>(
            _n,
            _script_engine.get(),
            _data_manager,
            _repl_state,
            [this](const std::wstring& line)
            { _interactive->process(line); });
    }

    ~Impl()
    {
        delete _n;
    }

    // Member function to delegate to CommandExecutor
    void process_command(const std::vector<std::wstring>& cmd) const;

    std::shared_ptr<DataManager>     _data_manager;
    network::Reasoning* const        _n;
    std::unique_ptr<ScriptEngine>    _script_engine;
    std::unique_ptr<CommandExecutor> _command_executor;
    std::shared_ptr<ReplState>       _repl_state;

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

private:
    const Interactive* _interactive;
};

console::Interactive::Interactive(OutputHandler output)
    : _pImpl(new Impl(this, std::move(output)))
{
}

console::Interactive::~Interactive()
{
    delete _pImpl;
}

void console::Interactive::process_file(const std::wstring& file, const std::vector<std::string>& args) const
{
    _pImpl->_command_executor->import_file(file, args);
}

std::string console::Interactive::get_version()
{
    return network::Zelph::get_version();
}

bool console::Interactive::is_auto_run_active() const
{
    return _pImpl->_repl_state->auto_run;
}

bool console::Interactive::is_accumulating() const
{
    const auto& s = _pImpl->_repl_state;
    return s->accumulating_zelph
        || s->accumulating_inline_janet
        || s->script_mode == ScriptMode::Janet;
}

void console::Interactive::process(std::wstring line) const
{
    try
    {
        // --- 1. Comments (work in all modes) ---
        if (boost::starts_with(line, L"#")) return;

        size_t first_char_pos = line.find_first_not_of(L" \t");

        // --- 2. Commands starting with '.' (work in all modes) ---
        if (first_char_pos != std::wstring::npos && line[first_char_pos] == L'.')
        {
            tokenizer<escaped_list_separator<wchar_t>, std::wstring::const_iterator, std::wstring> tok(line, escaped_list_separator<wchar_t>(L"\\", L" \t", L"\""));
            auto                                                                                   it = tok.begin();
            while (it != tok.end() && it->empty())
                ++it;

            if (it != tok.end() && (*it)[0] == L'.')
            {
                std::vector<std::wstring> cmd;
                while (it != tok.end())
                {
                    if (!it->empty()) cmd.push_back(*it);
                    ++it;
                }

                _pImpl->_n->profiler_reset_epoch();
                _pImpl->process_command(cmd);
                return;
            }
        }

        // --- 3. Empty lines ---
        if (first_char_pos == std::wstring::npos) return;

        auto& state = _pImpl->_repl_state;

        // --- 4. Accumulating an incomplete inline Janet expression (from a previous % line) ---
        if (state->accumulating_inline_janet)
        {
            std::string utf8_line = string::unicode::to_utf8(line);
            state->janet_buffer += utf8_line + "\n";

            if (zelph::ScriptEngine::is_expression_complete(state->janet_buffer))
            {
                _pImpl->_script_engine->process_janet(state->janet_buffer, false);
                state->janet_buffer.clear();
                state->accumulating_inline_janet = false;

                if (state->auto_run)
                    _pImpl->_n->run(true, false, false, true);
            }
            return;
        }

        std::string trimmed_utf8 = string::unicode::to_utf8(line);
        boost::trim(trimmed_utf8);

        // --- 5. Mode toggle: bare '%' on a line ---
        if (trimmed_utf8 == "%")
        {
            if (state->script_mode == ScriptMode::Janet)
            {
                // Leaving Janet block mode: execute accumulated code
                if (!state->janet_buffer.empty())
                {
                    _pImpl->_n->profiler_reset_epoch();
                    _pImpl->_script_engine->process_janet(state->janet_buffer, false);
                    state->janet_buffer.clear();

                    if (state->auto_run)
                        _pImpl->_n->run(true, false, false, true);
                }
                state->script_mode = ScriptMode::Zelph;
            }
            else
            {
                state->script_mode = ScriptMode::Janet;
            }
            return;
        }

        // --- 6. Inline Janet: '%' followed by code ---
        if (trimmed_utf8[0] == '%')
        {
            std::string janet_code = trimmed_utf8.substr(1);
            boost::trim_left(janet_code);

            if (janet_code.empty()) return;

            if (zelph::ScriptEngine::is_expression_complete(janet_code))
            {
                _pImpl->_n->profiler_reset_epoch();
                _pImpl->_script_engine->process_janet(janet_code, false);

                if (state->auto_run)
                    _pImpl->_n->run(true, false, false, true);
            }
            else
            {
                state->janet_buffer              = janet_code + "\n";
                state->accumulating_inline_janet = true;
            }
            return;
        }

        // --- 7. Janet block mode: accumulate lines ---
        if (state->script_mode == ScriptMode::Janet)
        {
            std::string utf8_line = string::unicode::to_utf8(line);
            state->janet_buffer += utf8_line + "\n";
            return;
        }

        // --- 8. Zelph mode: accumulate until statement is complete, then parse ---
        std::string utf8_line = string::unicode::to_utf8(line);

        if (state->accumulating_zelph)
            state->zelph_buffer += "\n" + utf8_line;
        else
            state->zelph_buffer = utf8_line;

        if (!zelph::ScriptEngine::is_zelph_complete(state->zelph_buffer))
        {
            state->accumulating_zelph = true;
            return;
        }

        std::string complete_stmt = state->zelph_buffer;
        state->zelph_buffer.clear();
        state->accumulating_zelph = false;

        std::string transformed = _pImpl->_script_engine->parse_zelph_to_janet(complete_stmt);

        if (!transformed.empty())
        {
            _pImpl->_n->profiler_reset_epoch();
            _pImpl->_script_engine->process_janet(transformed, true);
        }
        else
        {
            size_t u_first = complete_stmt.find_first_not_of(" \t\n");
            if (u_first != std::string::npos)
            {
                throw std::runtime_error("Syntax error: Could not parse statement.");
            }
        }

        if (state->auto_run)
        {
            _pImpl->_n->run(true, false, false, true);
        }
    }
    catch (std::exception& ex)
    {
        throw std::runtime_error("Error in line \"" + string::unicode::to_utf8(line) + "\": " + ex.what());
    }
}

void console::Interactive::import_file(const std::wstring& file) const
{
    _pImpl->_command_executor->import_file(file);
}

// Delegation method
void console::Interactive::Impl::process_command(const std::vector<std::wstring>& cmd) const
{
    _command_executor->execute(cmd);
}

void console::Interactive::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition) const
{
    _pImpl->_n->run(print_deductions, generate_markdown, suppress_repetition);
}

std::string console::Interactive::get_lang() const
{
    return _pImpl->_n->get_lang();
}

void console::Interactive::set_output_handler(OutputHandler output) const
{
    _pImpl->_n->set_output_handler(std::move(output));
}

void console::Interactive::out(const std::wstring& text, bool newline) const
{
    _pImpl->_n->emit(OutputChannel::Out, text, newline);
}

void console::Interactive::err(const std::wstring& text, bool newline) const
{
    _pImpl->_n->emit(OutputChannel::Error, text, newline);
}

void console::Interactive::log(const std::wstring& text, bool newline) const
{
    _pImpl->_n->emit(OutputChannel::Diagnostic, text, newline);
}

void console::Interactive::prompt(const std::wstring& text, bool newline) const
{
    _pImpl->_n->emit(OutputChannel::Prompt, text, newline);
}

#ifdef PROVIDE_C_INTERFACE
console::Interactive interactive;

extern "C" void zelph_process_c(const char* line, size_t len)
{
    if (len > 0)
    {
        std::string l(line, 0, len);
        interactive.process(string::unicode::from_utf8(l));
    }
}

extern "C" void zelph_run()
{
    interactive.run(true, false, false);
}
#endif
