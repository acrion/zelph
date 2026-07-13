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

if(NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_C_COMPILER AND NOT CMAKE_CXX_COMPILER)
    find_program(CLANG_EXE clang)
    find_program(CLANGXX_EXE clang++)
    if(CLANG_EXE AND CLANGXX_EXE)
        message(STATUS "Defaulting to Clang: ${CLANGXX_EXE}")
        set(CMAKE_C_COMPILER "${CLANG_EXE}")
        set(CMAKE_CXX_COMPILER "${CLANGXX_EXE}")
    endif()
endif()

project(zelph VERSION 0.9.9 LANGUAGES C CXX)
set(ZELPH_VERSION_SUFFIX "-dev" CACHE STRING "Version suffix appended to the reported project version")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(project_options INTERFACE)

option(ZELPH_BUILD_APP   "Build the zelph REPL executable" ${PROJECT_IS_TOP_LEVEL})
option(ZELPH_BUILD_TESTS "Build zelph tests"               ${PROJECT_IS_TOP_LEVEL})

# WebAssembly playground build. Implies: static lib, no Cap'n Proto
# persistence, no native app, no test suite. Use via: emcmake cmake ...
option(ZELPH_WASM "Build the WebAssembly playground" ${EMSCRIPTEN})

if(ZELPH_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "ZELPH_WASM requires the Emscripten toolchain (emcmake cmake ...)")
    endif()
    set(ZELPH_BUILD_APP OFF)
    set(ZELPH_BUILD_TESTS OFF)
endif()

if(MSVC)
    target_compile_options(project_options INTERFACE
        /W4 /wd4251 /wd4275 /utf-8
        $<$<CONFIG:Debug>:-D_DEBUG>
    )
else()
    if(EMSCRIPTEN)
        # Wasm exception handling: must be enabled at compile AND link time.
        # zelph relies on C++ exceptions (contradiction_error, REPL error
        # paths). -fwasm-exceptions uses native wasm EH instructions (near
        # zero cost on non-throwing paths) and also selects the matching
        # wasm-EH based setjmp/longjmp mode that the Janet VM relies on.
        target_compile_options(project_options INTERFACE
                -fwasm-exceptions -Wall -pedantic
            )
        target_link_options(project_options INTERFACE -fwasm-exceptions)
    else()
        target_compile_options(project_options INTERFACE
                -fexceptions -Wall -pedantic
            )
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(project_options INTERFACE -g)
    endif()
endif()

add_subdirectory(src/lib)

if(ZELPH_WASM)
    add_subdirectory(src/wasm)
endif()

if(ZELPH_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(ZELPH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(src/test)
endif()

if(PROJECT_IS_TOP_LEVEL)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME})

    # Install the zelph standard library (everything under stdlib/) into the
    # platform data directory, e.g. /usr/share/zelph. No file list is
    # maintained: whatever lives in stdlib/ is part of the standard library.
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
                    DESTINATION ${CMAKE_INSTALL_DATADIR}/zelph)
endif()
