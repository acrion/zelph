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

add_executable(zelph main.cpp)

set(ZELPH_APP_LIBS project_options)

if(TARGET zelph_mimalloc_override)
    list(APPEND ZELPH_APP_LIBS zelph_mimalloc_override)
endif()

message(STATUS "CMAKE_CXX_COMPILER_ID = ${CMAKE_CXX_COMPILER_ID}")

if (MSVC)
    target_compile_definitions(zelph PRIVATE BOOST_LIB_DIAGNOSTIC)
    list(APPEND ZELPH_APP_LIBS zelph_lib)

    add_custom_command(TARGET zelph POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:zelph>
                $<TARGET_FILE_DIR:zelph>
        COMMAND_EXPAND_LISTS
    )
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    list(APPEND ZELPH_APP_LIBS Threads::Threads zelph_lib)
else ()
    list(APPEND ZELPH_APP_LIBS zelph_lib)
endif ()

target_link_libraries(zelph PRIVATE ${ZELPH_APP_LIBS})

# Mirror the standard library next to the binary so that freshly built
# binaries resolve `.import <name>` out of the box and the CI zip archives
# (which package the bin directory) contain it.
#
# The mirror depends on the stdlib SOURCES, not merely on the binary: as a
# POST_BUILD step it only ran when the target relinked, so editing a .zph
# file and rebuilding copied nothing and the binary silently kept using the
# previous stdlib. It also depends on the binary itself, so a relink into a
# cleaned output directory restores the mirror.
#
# It is an IMAGE of the source tree, not an overlay: the destination is
# removed first. Plain copying leaves behind files that no longer exist in
# stdlib/, and an orphan there is worse than a missing file -- `.import
# <name>` finds it, so nothing indicates that the definition is months out
# of date. The rename arithmetic.zph -> decimal-arithmetic.zph left exactly
# such an orphan, and the test suite went on importing it.
file(GLOB_RECURSE ZELPH_STDLIB_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/stdlib/*)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stdlib_mirror.stamp
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:zelph>/stdlib
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/stdlib
            $<TARGET_FILE_DIR:zelph>/stdlib
    COMMAND ${CMAKE_COMMAND} -E touch
            ${CMAKE_CURRENT_BINARY_DIR}/stdlib_mirror.stamp
    DEPENDS ${ZELPH_STDLIB_SOURCES} $<TARGET_FILE:zelph>
    COMMENT "Mirroring zelph standard library next to the binary")

add_custom_target(zelph_stdlib_mirror ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/stdlib_mirror.stamp)

install(TARGETS zelph RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
