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

#pragma once

// A stand-in for curl, used in tests that load straight from the repository.
// The engine fetches from Hugging Face by starting curl as a program, with
// ZELPH_HF_CURL_BIN specifying an alternative executable. No test may reach the
// network, so the program named there serves a local directory under the URLs
// that hf://datasets/acrion/zelph-partial-load-test/ uses. That program is the
// test binary itself: its main() delegates to serve() when the environment
// names a repository. A script in the temp directory would need to be executed
// from within it, which fails if that directory is mounted without execute
// permissions; the test binary runs wherever the tests execute, and no
// additional installation is required beyond the binary.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace zelph::test::curl_stand_in
{
    // The directory containing the files that are served. A test sets it for
    // the engine it drives, and the stand-in locates it within the environment
    // inherited by the engine's command.
    inline constexpr const char* repository_variable = "ZELPH_TEST_HF_REPOSITORY";

    // When set, a probe is responded to exactly as after a redirect: the
    // header block of the redirect, which claims this many bytes, precedes
    // the block of the object itself.
    inline constexpr const char* redirect_variable = "ZELPH_TEST_HF_REDIRECT_LENGTH";

    // The file that a shell starts for `name`, the name under which a program
    // was launched: the name itself, made absolute, if it has a directory,
    // otherwise the first file matching that name within the directories
    // listed in PATH. Empty if there is none.
    inline std::filesystem::path locate(const std::string& name)
    {
        namespace fs = std::filesystem;
        std::error_code ignored;
        if (fs::path(name).has_parent_path()) return fs::absolute(name, ignored).lexically_normal();

        const char* const  directories = std::getenv("PATH");
        std::istringstream list(directories == nullptr ? "" : directories);
        for (std::string directory; std::getline(list, directory, ':');)
        {
            const fs::path candidate = fs::path(directory.empty() ? "." : directory) / name;
            if (fs::is_regular_file(candidate, ignored)) return fs::absolute(candidate, ignored).lexically_normal();
        }
        return {};
    }

    // The location of the running test binary, which is what a test names in
    // ZELPH_HF_CURL_BIN. main() captures it before any test runs, while the
    // current working directory remains the one from which the binary was
    // launched.
    inline std::filesystem::path& program()
    {
        static std::filesystem::path path;
        return path;
    }

    // When set, a probe is responded to without including the object's
    // size, akin to a server that sends the object in fragments without
    // declaring the overall length.
    inline constexpr const char* no_length_variable = "ZELPH_TEST_HF_NO_LENGTH";

    // Responds to one request in the manner of curl, as far as the engine reads
    // the answer: a probe (-D) gets the file's headers, including its size and
    // a tag that changes with the content; a fetch (-o) gets the file, or the
    // range of it specified by --range; and standard output gets the line that
    // --write-out requests. Returns the same exit status that curl would
    // return.
    inline int serve(const int argc, char** argv, const std::filesystem::path& repository, const char* const redirect_length)
    {
        const std::string prefix = "https://huggingface.co/datasets/acrion/zelph-partial-load-test/resolve/main/";
        std::string       headers, output, range, url;
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            const bool        has_next = i + 1 < argc;
            if (argument == "-D" && has_next)
                headers = argv[++i];
            else if (argument == "-o" && has_next)
                output = argv[++i];
            else if (argument == "--range" && has_next)
                range = argv[++i];
            else if (argument == "--retry" || argument == "--connect-timeout" || argument == "--max-time" || argument == "--retry-max-time"
                     || argument == "--speed-limit" || argument == "--speed-time" || argument == "--write-out")
                ++i; // an option bearing a value that the stand-in cannot utilize
            else if (argument.empty() || argument.front() != '-')
                url = argument;
        }

        std::ifstream file;
        if (url.compare(0, prefix.size(), prefix) == 0) file.open(repository / url.substr(prefix.size()), std::ios::binary);
        if (!file.is_open())
        {
            std::puts("{\"http_code\":404,\"exitcode\":22}");
            return 22;
        }
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        if (!headers.empty())
        {
            std::uint64_t tag = 1469598103934665603ULL;
            for (const unsigned char byte : content)
                tag = (tag ^ byte) * 1099511628211ULL;

            std::ofstream out(headers, std::ios::binary);
            if (redirect_length != nullptr && *redirect_length != '\0')
            {
                out << "HTTP/1.1 302 Found\r\ncontent-length: " << redirect_length << "\r\nlocation: " << url << "\r\n\r\n";
            }
            out << "HTTP/1.1 200 OK\r\netag: \"" << tag << "\"\r\n";
            if (std::getenv(no_length_variable) == nullptr) out << "content-length: " << content.size() << "\r\n";
            out << "\r\n";
            std::puts("{\"http_code\":200,\"exitcode\":0}");
            return 0;
        }

        std::size_t first = 0;
        std::size_t count = content.size();
        if (const auto dash = range.find('-'); dash != std::string::npos)
        {
            first = static_cast<std::size_t>(std::stoull(range.substr(0, dash)));
            count = static_cast<std::size_t>(std::stoull(range.substr(dash + 1))) - first + 1;
        }
        first = first < content.size() ? first : content.size();
        count = count < content.size() - first ? count : content.size() - first;
        std::ofstream(output, std::ios::binary).write(content.data() + first, static_cast<std::streamsize>(count));
        std::printf("{\"http_code\":200,\"exitcode\":0,\"size_download\":%llu}\n", static_cast<unsigned long long>(count));
        return 0;
    }
}
