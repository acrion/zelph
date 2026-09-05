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

    // Whether standard input is a person typing.
    //
    // It decides the one thing a terminal and a script genuinely differ in:
    // whether there is input ALREADY COMMITTED that must not run once a line
    // has failed. A file and a pipe have that -- the rest of the script was
    // written before the failure and now rests on a premise that did not hold,
    // which is how a script whose `.import` did not resolve went on answering
    // plausibly for six weeks. A terminal has none: the next line does not
    // exist yet, and its author has just read the error.
    bool stdin_is_a_terminal()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) != 0;
#else
        return isatty(STDIN_FILENO) != 0;
#endif
    }

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

        // A script run prints no prompt, so the mark the default deduction
        // mode uses has nowhere to appear and a run says in words what it hid.
        if (!script_files.empty()) interactive.set_prompt_available(false);

        for (const auto& file : script_files)
        {
            try
            {
                interactive.process_file(file, script_args);
            }
            catch (const std::exception& e)
            {
                interactive.err(e.what());
                interactive.note_failure();
                interactive.err("zelph: '" + file + "' stopped at the error above; the lines after it did not run.");
            }
        }

        if (script_files.empty())
        {
            std::string exit_command = ".quit";
            interactive.out("zelph " + zelph::console::Interactive::get_version());
            // One line, and it stays one line. What a first-time reader needs
            // is where the help is, and one example worth following -- the
            // deduction filter, because it is the only default that WITHHOLDS
            // something, and the "+" it puts in the prompt is the one mark
            // nothing else explains.
            interactive.out("-- REPL mode - type .help for commands, e.g. '.help .deductions' to change what is shown --");
            interactive.out("");

            const bool typed     = stdin_is_a_terminal();
            bool       broke_off = false;

            interactive.prompt(interactive.prompt_text(), false);

            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line == exit_command)
                    break;

                if (line.empty() && !interactive.is_accumulating())
                {
                    interactive.out("type .help for help --");
                    interactive.prompt(interactive.prompt_text(), false);
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
                    interactive.note_failure();

                    // Piped input is a script, and a script stops where it
                    // failed -- the same rule as for a file named on the
                    // command line, so the two forms of running one no longer
                    // disagree about what a failure costs. A terminal carries
                    // on: there is nothing after the failing line yet.
                    if (!typed)
                    {
                        interactive.err("zelph: stopped at the error above; the input after it was not read.");
                        broke_off = true;
                        break;
                    }
                }

                const auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed >= kReplTimingThreshold)
                {
                    interactive.log("-- " + format_duration(elapsed) + " --");
                }

                interactive.prompt(interactive.prompt_text(), false);
            }

            // End of input is the last chance to dispatch a half-read block.
            // Without this, "printf '...sparql\nSELECT ...' | zelph" ran the
            // query when it came from a FILE and silently dropped it when it
            // came from stdin. Skipped after a break-off, where whatever is
            // half-read belongs to input that will not run anyway, and
            // reporting it would add a second error about the first one.
            if (!broke_off)
            {
                try
                {
                    interactive.finish_input();
                }
                catch (const std::exception& e)
                {
                    interactive.err(e.what());
                    interactive.note_failure();
                }
            }

            interactive.out("");
        }
    }
    catch (std::exception& ex)
    {
        interactive.err(ex.what());
        interactive.note_failure();
    }

    if (interactive.had_failure())
    {
        interactive.err("zelph: finished with errors, see above.");
        return 1;
    }

    return 0;
}
