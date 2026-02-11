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

target_link_libraries(zelph PRIVATE project_options)

install(TARGETS zelph RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "CMAKE_CXX_COMPILER_ID = ${CMAKE_CXX_COMPILER_ID}")

if (MSVC)
    target_compile_definitions(zelph PRIVATE BOOST_LIB_DIAGNOSTIC)
    target_link_libraries(zelph PRIVATE zelph_lib)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(zelph PRIVATE Threads::Threads zelph_lib stdc++fs)
else ()
    target_link_libraries(zelph PRIVATE zelph_lib)
endif ()
