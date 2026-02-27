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
#include "string_utils.hpp"

#ifndef _WIN32
    #include <cstdlib>     // for system
    #include <sys/types.h> // for fork
    #include <sys/wait.h>  // for waitpid
    #include <unistd.h>    // for execvp, getenv
#endif

#include <iostream>
#include <vector>

using namespace zelph::console;
Interactive interactive;

int main(int argc, char** argv)
{
#if !defined(_WIN32) && defined(NDEBUG)
    if (getenv("ZELPH_UNDER_RLWRAP") == nullptr)
    {
        FILE* pipe = popen("command -v rlwrap", "r");
        if (pipe)
        {
            char        buffer[128];
            std::string rlwrap_path;
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
            {
                rlwrap_path += buffer;
            }
            int status = pclose(pipe);
            if (status == 0 && !rlwrap_path.empty())
            {
                setenv("ZELPH_UNDER_RLWRAP", "1", 1);
                std::vector<char*> exec_args;
                exec_args.push_back(const_cast<char*>("rlwrap"));
                exec_args.push_back(const_cast<char*>("-m"));
                exec_args.push_back(argv[0]);
                for (int i = 1; i < argc; ++i)
                {
                    exec_args.push_back(argv[i]);
                }
                exec_args.push_back(nullptr);
                execvp("rlwrap", exec_args.data());
                perror("execvp failed");
                return 1;
            }
        }
    }
#endif
    try
    {
        std::wstring              exit_command = L".exit";
        std::vector<std::wstring> script_files;
        bool                      show_version = false;

        std::vector<std::string> script_args;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "-v" || arg == "--version")
            {
                show_version = true;
            }
            else if (script_files.empty())
            {
                script_files.push_back(zelph::string::unicode::from_utf8(arg));
            }
            else
            {
                script_args.push_back(arg);
            }
        }

        if (show_version)
        {
#ifdef _WIN32
            std::wcout << L"zelph " << zelph::string::unicode::from_utf8(interactive.get_version()) << std::endl;
#else
            std::cout << "zelph " << zelph::console::Interactive::get_version() << std::endl;
#endif
            return 0;
        }

        for (const auto& file : script_files)
        {
            interactive.process_file(file, script_args);
        }

        if (script_files.empty())
        {
#ifdef _WIN32
            std::wcout << L"zelph " << zelph::string::unicode::from_utf8(interactive.get_version()) << std::endl;
#else
            std::cout << "zelph " << zelph::console::Interactive::get_version() << std::endl;
#endif

            std::wcout << L"-- REPL mode - type .help for commands, " << exit_command << L" to exit --" << std::endl;
            std::wcout << std::endl;

            auto make_prompt = [&]() -> std::wstring
            {
                if (interactive.is_accumulating())
                    return L"";
                else
                    return zelph::string::unicode::from_utf8(interactive.get_lang())
                         + (interactive.is_auto_run_active() ? L"> " : L"-> ");
            };

            std::wcout << make_prompt();
            std::wcout.flush();

            std::wstring line;
            while (std::getline(std::wcin, line))
            {
                if (line == exit_command)
                    break;

                if (line.empty())
                {
                    std::wcout << L"type .help for help --" << std::endl;
                    std::wcout << make_prompt();
                    std::wcout.flush();
                    continue;
                }

                try
                {
                    interactive.process(line);
                }
                catch (const std::exception& e)
                {
                    std::wcerr << zelph::string::unicode::from_utf8(e.what()) << std::endl;
                }

                std::wcout << make_prompt();
                std::wcout.flush();
            }

            std::wcout << std::endl;
        }
    }
    catch (std::exception& ex)
    {
        std::wcout << zelph::string::unicode::from_utf8(ex.what()) << std::endl;
    }

    return 0;
}
