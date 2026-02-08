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
#include "network.hpp"
#include "platform_utils.hpp"
#include "reasoning.hpp"
#include "repl_state.hpp"
#include "script_engine.hpp"
#include "string_utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>

#include <fstream>
#include <iostream>

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
    Impl(Interactive* enclosing)
        : _n(new network::Reasoning([](const std::wstring& str, const bool)
                                    {
#ifdef _WIN32
                                        std::wcout << str << std::endl;
#else
                                        std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
                                    }))
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
        _n->register_core_node(_n->core.FollowedBy, L"..");
        _n->register_core_node(_n->core.PartOf, L"in");
        _n->register_core_node(_n->core.Conjunction, L"conjunction");

        _script_engine->initialize();

        // Initialize CommandExecutor with references to our state
        _command_executor.reset(new CommandExecutor(
            _n,
            _script_engine.get(),
            _data_manager,
            _repl_state,
            [this](const std::wstring& line)
            { _interactive->process(line); }));
    }

    ~Impl()
    {
        delete _n;
    }

    void import_file(const std::wstring& file) const;
    void process_zelph_file(const std::string& path, const std::vector<std::string>& args = {}) const;

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

// Helper for suppressing auto-run during batch operations (imports)
struct AutoRunSuspender
{
    std::shared_ptr<console::ReplState> state;
    bool                                previous_val;

    AutoRunSuspender(std::shared_ptr<console::ReplState> s)
        : state(s)
    {
        previous_val    = state->auto_run;
        state->auto_run = false;
    }
    ~AutoRunSuspender()
    {
        state->auto_run = previous_val;
    }
};

console::Interactive::Interactive()
    : _pImpl(new Impl(this))
{
}

console::Interactive::~Interactive()
{
    delete _pImpl;
}

void console::Interactive::Impl::import_file(const std::wstring& file) const
{
    AutoRunSuspender suspend(_repl_state); // Don't auto-run inside scripts

    std::clog << "Importing file " << string::unicode::to_utf8(file) << "..." << std::endl;
    std::wifstream stream(string::unicode::to_utf8(file));

    if (stream.fail()) throw std::runtime_error("Could not open file '" + string::unicode::to_utf8(file) + "'");

    for (std::wstring line; std::getline(stream, line);)
    {
        _interactive->process(line);
    }
}

void console::Interactive::Impl::process_zelph_file(const std::string& path, const std::vector<std::string>& args) const
{
    AutoRunSuspender suspend(_repl_state); // Don't auto-run inside scripts
    _script_engine->set_script_args(args);

    std::ifstream file(path);
    if (!file) throw std::runtime_error("Konnte Datei nicht öffnen: " + path);
    std::string line;
    while (std::getline(file, line))
    {
        boost::trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::wstring wline = string::unicode::from_utf8(line);
        _interactive->process(wline);
    }
}

void console::Interactive::process_file(const std::wstring& file, const std::vector<std::string>& args) const
{
    _pImpl->process_zelph_file(string::unicode::to_utf8(file), args);
}

std::string console::Interactive::get_version() const
{
    return network::Zelph::get_version();
}

bool console::Interactive::is_auto_run_active() const
{
    return _pImpl->_repl_state->auto_run;
}

void console::Interactive::process(std::wstring line) const
{
    _pImpl->_n->set_print([](const std::wstring& str, bool)
                          {
#ifdef _WIN32
                              std::wcout << string::unmark_identifiers(str) << std::endl;
#else
                              std::clog << string::unicode::to_utf8(string::unmark_identifiers(str)) << std::endl;
#endif
                          });

    bool was_command_or_empty = false;

    try
    {
        if (boost::starts_with(line, "#")) return; // comment

        size_t first_char_pos = line.find_first_not_of(L" \t");
        if (first_char_pos != std::wstring::npos && line[first_char_pos] == L'.')
        {
            was_command_or_empty = true;
            // Command processing logic deliberately does not use PEG
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

                _pImpl->process_command(cmd);
                return;
            }
        }
        else if (first_char_pos == std::wstring::npos)
        {
            was_command_or_empty = true;
        }

        std::string utf8_line   = string::unicode::to_utf8(line);
        std::string transformed = _pImpl->_script_engine->parse_zelph_to_janet(utf8_line);

        // 1. Zelph syntax
        if (!transformed.empty())
        {
            _pImpl->_script_engine->process_janet(transformed, true);
        }

        // 2. Native Janet syntax
        else
        {
            size_t u_first = utf8_line.find_first_not_of(" \t");
            if (u_first != std::string::npos && utf8_line[u_first] == '(')
            {
                _pImpl->_script_engine->process_janet(utf8_line, false);
            }
            // 3. Syntax Error
            else if (u_first != std::string::npos) // Line is not empty
            {
                throw std::runtime_error("Syntax error: Could not parse line.");
            }
        }

        // Auto-run logic: Only if auto_run is on, and it wasn't a command or comment
        if (_pImpl->_repl_state->auto_run && !was_command_or_empty)
        {
            // silent = true to suppress standard reasoning logging
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
    _pImpl->import_file(file);
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
