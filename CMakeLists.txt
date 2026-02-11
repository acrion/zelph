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

cmake_minimum_required(VERSION 3.25.2)

if(NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER)
    find_program(CLANG_EXE clang)
    find_program(CLANGXX_EXE clang++)
    if(CLANG_EXE AND CLANGXX_EXE)
        message(STATUS "Defaulting to Clang: ${CLANGXX_EXE}")
        set(CMAKE_C_COMPILER "${CLANG_EXE}")
        set(CMAKE_CXX_COMPILER "${CLANGXX_EXE}")
    endif()
endif()

project(zelph VERSION 0.9.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Wichtig für KDevelop/LSP

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(project_options INTERFACE)

if(MSVC)
    target_compile_options(project_options INTERFACE
        /W4 /wd4251 /wd4275 /utf-8
        $<$<CONFIG:Debug>:-D_DEBUG>
    )
else()
    target_compile_options(project_options INTERFACE
        -fexceptions -Wall -pedantic
    )

    # Debug Info
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(project_options INTERFACE -g)
    endif()
endif()

add_subdirectory(src/lib)
add_subdirectory(src/app)

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME})
