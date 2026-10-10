# Copyright (c) 2025, 2026 acrion innovations GmbH
# Authors: Stefan Zipproth, s.zipproth@acrion.ch
#
# This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org
#
# zelph is offered under a commercial and under the AGPL license.
# For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.
#
# AGPL licensing:
#
# zelph is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# zelph is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with zelph. If not, see <https://www.gnu.org/licenses/>.

# Run with -P. Writes OUTPUT, a C++ source defining FUNCTION in namespace
# zelph::test, which returns the bytes of every *.bin file in INPUT_DIR under
# its name without the extension.

file(GLOB inputs "${INPUT_DIR}/*.bin")
list(SORT inputs)

set(source "// Generated from ${INPUT_DIR} by embed_files.cmake.\n\n")
string(APPEND source "#include <map>\n#include <string>\n#include <vector>\n\n")
string(APPEND source "namespace zelph::test\n{\n")
string(APPEND source "    const std::map<std::string, std::vector<unsigned char>>& ${FUNCTION}()\n    {\n")
string(APPEND source "        static const std::map<std::string, std::vector<unsigned char>> files{\n")
foreach(input IN LISTS inputs)
    get_filename_component(name "${input}" NAME_WE)
    file(READ "${input}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(APPEND source "            {\"${name}\", {${bytes}}},\n")
endforeach()
string(APPEND source "        };\n        return files;\n    }\n}\n")

file(WRITE "${OUTPUT}" "${source}")
