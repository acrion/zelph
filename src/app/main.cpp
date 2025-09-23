/*
Copyright (c) 2025 acrion innovations GmbH
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
#include "utils.hpp"

#include <codecvt>
#include <fstream>
#include <iostream>

using namespace zelph::console;
Interactive interactive;

int main(int argc, char** argv)
{
    try
    {
        std::wstring exit_command = L".exit";

        for (int i = 1; i < argc; ++i)
        {
            std::wcout << L"Importing file " << zelph::network::utils::wstr(argv[i]) << L"..." << std::endl;
            std::wifstream stream(argv[i]);

            if (stream.fail()) throw std::runtime_error(std::string("Could not open file '") + argv[i] + "'");

            for (std::wstring line; std::getline(stream, line);)
            {
                interactive.process(line);
            }
            interactive.run(true, false, false);
        }

        if (argc > 1) std::wcout << L"Ready." << std::endl;
        std::wcout << "zelph " << zelph::network::utils::wstr(Interactive::get_version()) << std::endl;
        std::wcout << std::endl;
        if (argc == 1) std::wcout << "You may specify script files that will be processed prior interactive mode." << std::endl;
        std::wcout << L" -- interactive mode - type " << exit_command << L" to exit --" << std::endl;
        std::wcout << std::endl;
        for (std::wstring line; std::getline(std::wcin, line) && line != exit_command;)
        {
            if (line.empty())
                std::wcout << "type " << exit_command << L" to exit --" << std::endl;
            else
                try
                {
                    interactive.process(line);
                }
                catch (const std::exception& e)
                {
                    std::wcerr << zelph::network::utils::wstr(e.what()) << std::endl;
                }
        }

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
        std::wcout << zelph::network::utils::wstr(ex.what()) << std::endl;
    }
}
