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

#include <fstream>
#include <iostream>
#include <vector>

using namespace zelph::console;
Interactive interactive;

int main(int argc, char** argv)
{
    try
    {
        std::wstring              exit_command = L".exit";
        std::vector<std::wstring> script_files;
        bool                      show_version = false;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "-v" || arg == "--version")
            {
                show_version = true;
            }
            else
            {
                script_files.push_back(zelph::string::unicode::from_utf8(arg));
            }
        }

        if (show_version)
        {
            std::cout << "zelph " << zelph::console::Interactive::get_version() << std::endl;
            return 0;
        }

        for (const auto& file : script_files)
        {
            interactive.import_file(file);
        }

        if (!script_files.empty())
            std::wcout << L"Ready." << std::endl;

        std::cout << "zelph " << zelph::console::Interactive::get_version() << std::endl;
        std::wcout << std::endl;

        if (script_files.empty())
            std::wcout << L"You may specify script files that will be processed before entering interactive mode." << std::endl;

        std::wcout << L"-- interactive mode - type .help for commands, " << exit_command << L" to exit --" << std::endl;
        std::wcout << std::endl;

        std::wstring line;
        std::wstring lang_prompt = zelph::string::unicode::from_utf8(interactive.get_lang());
        std::wcout << lang_prompt << L"> ";
        std::wcout.flush();

        while (std::getline(std::wcin, line))
        {
            if (line == exit_command)
                break;

            if (line.empty())
            {
                std::wcout << L"type .help for help --" << std::endl;
                lang_prompt = zelph::string::unicode::from_utf8(interactive.get_lang());
                std::wcout << lang_prompt << L"> ";
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

            lang_prompt = zelph::string::unicode::from_utf8(interactive.get_lang());
            std::wcout << lang_prompt << L"> ";
            std::wcout.flush();
        }

        std::wcout << std::endl;
#if 0
        n.set_name(n.core.RelationTypeCategory, L"Relationstyp", "de");
        n.set_name(n.core.Causes,               L"=>",           "de");
        n.set_name(n.core.And,                  L",",            "de");
        n.set_name(n.core.IsA,                  L"ist ein",      "de");
        n.set_name(n.core.Unequal,              L"ungleich",     "de");
        n.set_name(n.core.Contradiction,        L"Widerspruch",  "de");

        //n.fact(n.node(L"ist Eigenschaft von", "de"), n.node(L"ist Gegenteil von", "de"),   { n.node(L"hat die Eigenschaft", "de") });
        //n.fact(n.node(L"ist Teil von", "de"),        n.node(L"ist Gegenteil von", "de"),   { n.node(L"hat den Bestandteil", "de") });
        //n.fact(n.node(L"ist zum Beispiel", "de"),    n.node(L"ist Gegenteil von", "de"),   { n.node(L"ist ein",             "de") });
        n.fact(n.node(L"hat den Bestandteil", "de"), n.node(L"hat die Eigenschaft", "de"), { n.node(L"transitiv",           "de") });
        //n.fact(n.node(L"hat die Eigenschaft", "de"), n.node(L"hat die Eigenschaft", "de"), { n.node(L"transitiv",           "de") });
        //n.fact(n.node(L"ist ein", "de"),             n.node(L"hat die Eigenschaft", "de"), { n.node(L"transitiv",           "de") });

        {
            Node X = n.var();
            Node R = n.var();
            Node Y = n.var();
            Node Z = n.var();
            Node R_is_transitive = n.fact(R, n.node(L"hat die Eigenschaft", "de"), { n.node(L"transitiv", "de") });
            Node condition = n.condition(n.core.And, { R_is_transitive, n.fact(X, R, {Y}), n.fact(Y, R, {Z}) });
            n.fact(condition, n.core.Causes, { n.fact(X, R, {Z}) });
        }

        n.fact(n.node(L"Haus", "de"), n.node(L"hat den Bestandteil", "de"), { n.node(L"T\xfcr", "de") });
        n.fact(n.node(L"T\xfcr", "de"), n.node(L"hat den Bestandteil", "de"), { n.node(L"Griff", "de") });
        n.gen_dot(n.core.RelationTypeCategory, "test.dot");

        n.run();
        std::wcout << L"Ready." << std::endl;
        n.debug();
#endif
    }
    catch (std::exception& ex)
    {
        std::wcout << zelph::string::unicode::from_utf8(ex.what()) << std::endl;
    }

    return 0;
}
