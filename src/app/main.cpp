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
#include "versions.hpp"

#ifdef _WIN32
    #include <Windows.h> // for SetConsoleOutputCP
    #include <fcntl.h>   // for _O_U16TEXT
    #include <io.h>      // for _setmode
    #include <stdio.h>   // for _fileno
#else
    #include <sys/types.h> // for fork
    #include <sys/wait.h>  // for waitpid
    #include <unistd.h>    // for execvp, getenv
#endif

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    // Print a timing line after a REPL input only when it took noticeable
    // time. A script import (.import file) arrives as a single REPL line,
    // so it yields exactly one timing - never one per script line.
    constexpr std::chrono::milliseconds kReplTimingThreshold{10};

    std::string format_duration(const std::chrono::steady_clock::duration d)
    {
        using namespace std::chrono;
        const long long ms = duration_cast<milliseconds>(d).count();
        if (ms < 1000) return std::to_string(ms) + " ms";

        const long long s = ms / 1000;
        char            buf[64];
        if (s < 60)
            std::snprintf(buf, sizeof(buf), "%lld.%03lld s", s, ms % 1000);
        else
            std::snprintf(buf, sizeof(buf), "%lldm%lld.%03llds", s / 60, s % 60, ms % 1000);
        return buf;
    }
}

using namespace zelph::console;
Interactive interactive;

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try
    {
        std::vector<std::string> script_files;
        bool                     show_version = false;

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
                script_files.push_back(arg);
            }
            else
            {
                script_args.push_back(arg);
            }
        }

#if !defined(_WIN32) && defined(NDEBUG)
        // rlwrap exists solely for the interactive REPL (line editing,
        // history). Skip it for script runs (zelph <script>), for --version,
        // and when stdin is not a terminal - e.g. when another program (a
        // chess GUI speaking UCI, a test driver) controls zelph via pipes.
        if (script_files.empty() && !show_version && isatty(STDIN_FILENO)
            && getenv("ZELPH_NO_RLWRAP") == nullptr)
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

        if (show_version)
        {
            std::cout << zelph::get_version_description() << std::endl;
            return 0;
        }

        for (const auto& file : script_files)
        {
            interactive.process_file(file, script_args);
        }

        if (script_files.empty())
        {
            std::string exit_command = ".quit";
            interactive.out("zelph " + zelph::console::Interactive::get_version());
            interactive.out("-- REPL mode - type .help for commands, " + exit_command + " to exit --");
            interactive.out("");

            auto make_prompt = [&]() -> std::string
            {
                if (interactive.is_accumulating())
                    return "";
                else
                    return interactive.get_lang()
                         + (interactive.is_auto_run_active() ? "> " : "-> ");
            };

            interactive.prompt(make_prompt(), false);

            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line == exit_command)
                    break;

                if (line.empty() && !interactive.is_accumulating())
                {
                    interactive.out("type .help for help --");
                    interactive.prompt(make_prompt(), false);
                    continue;
                }

                const auto start_time = std::chrono::steady_clock::now();

                try
                {
                    interactive.process(line);
                }
                catch (const std::exception& e)
                {
                    interactive.err(e.what());
                }

                const auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed >= kReplTimingThreshold)
                {
                    interactive.log("-- " + format_duration(elapsed) + " --");
                }

                interactive.prompt(make_prompt(), false);
            }

            interactive.out("");
        }
    }
    catch (std::exception& ex)
    {
        interactive.err(ex.what());
    }

    return 0;
}
