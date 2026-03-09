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

#ifdef _WIN32
    #include <fcntl.h> // for _O_U16TEXT
    #include <io.h>    // for _setmode
    #include <stdio.h> // for _fileno
#else
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
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_U16TEXT);
#elif defined(NDEBUG)
    if (getenv("ZELPH_NO_RLWRAP") == nullptr)
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
                setenv("ZELPH_NO_RLWRAP", "1", 1);
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
            interactive.out(L"zelph " + zelph::string::unicode::from_utf8(zelph::console::Interactive::get_version()));

            return 0;
        }

        for (const auto& file : script_files)
        {
            interactive.process_file(file, script_args);
        }

        if (script_files.empty())
        {
            interactive.out(L"zelph " + zelph::string::unicode::from_utf8(zelph::console::Interactive::get_version()));
            interactive.out(L"-- REPL mode - type .help for commands, " + exit_command + L" to exit --");
            interactive.out(L"");

            auto make_prompt = [&]() -> std::wstring
            {
                if (interactive.is_accumulating())
                    return L"";
                else
                    return zelph::string::unicode::from_utf8(interactive.get_lang())
                         + (interactive.is_auto_run_active() ? L"> " : L"-> ");
            };

            interactive.prompt(make_prompt(), false);

            std::string line8;
            while (std::getline(std::cin, line8))
            {
                std::wstring line = zelph::string::unicode::from_utf8(line8);
                if (line == exit_command)
                    break;

                if (line.empty())
                {
                    interactive.out(L"type .help for help --");
                    interactive.prompt(make_prompt(), false);
                    continue;
                }

                try
                {
                    interactive.process(line);
                }
                catch (const std::exception& e)
                {
                    interactive.err(zelph::string::unicode::from_utf8(e.what()));
                }

                interactive.prompt(make_prompt(), false);
            }

            interactive.out(L"");
        }
    }
    catch (std::exception& ex)
    {
        interactive.err(zelph::string::unicode::from_utf8(ex.what()));
    }

    return 0;
}
