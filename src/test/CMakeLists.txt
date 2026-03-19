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

include(FetchContent)

# doctest v2.4.11 declares cmake_minimum_required(VERSION 3.1),
# which newer CMake rejects. Allow it to configure anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    SYSTEM
)
FetchContent_MakeAvailable(doctest)

add_executable(zelph_tests test_interactive.cpp)
target_link_libraries(zelph_tests PRIVATE zelph_lib doctest_with_main)

# suppress doctest specific __COUNTER__ warnings
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(zelph_tests PRIVATE -Wno-c2y-extensions -Wno-\#warnings)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(zelph_tests PRIVATE -Wno-cpp)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    find_package(Threads REQUIRED)
    target_link_libraries(zelph_tests PRIVATE Threads::Threads)
endif()

add_test(NAME zelph_tests COMMAND zelph_tests)
