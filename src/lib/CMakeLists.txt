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
cmake_policy(VERSION 3.25.2)

option(USE_CLANG "Build with clang instead of default compiler" OFF)

if (USE_CLANG)
    set(CMAKE_C_COMPILER "clang")
    set(CMAKE_CXX_COMPILER "clang++")
    message(STATUS "Using Clang compiler")
endif ()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(OPTIMIZATION_FLAGS "-O3")

    if (COMPILER_SUPPORTS_MARCH_NATIVE)
        set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -march=native")
    endif ()

    set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -flto")

    set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -ffast-math -funroll-loops")

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -fomit-frame-pointer")

        if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9.0)
            set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -fno-semantic-interposition")
        endif ()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -fomit-frame-pointer")
    endif ()

    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OPTIMIZATION_FLAGS}")
    message(STATUS "Optimization flags: ${OPTIMIZATION_FLAGS}")
elseif (MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Oi /Ot /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG")
endif ()

include(FetchContent)
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.8.1
)

FetchContent_MakeAvailable(unordered_dense)

#FetchContent_Declare(
#    acrion_cmake
#    GIT_REPOSITORY git@github.com:acrion/cmake.git
#    GIT_TAG main
#)
#FetchContent_MakeAvailable(acrion_cmake)
#
#include(${acrion_cmake_SOURCE_DIR}/set-cpp-version.cmake)

add_library(zelph SHARED
    # zelph::network
    answer.cpp
    answer.hpp
    zelph.cpp
    zelph.hpp
    zelph_impl.hpp
    contradiction_error.hpp
    markdown.cpp
    markdown.hpp
    network.hpp
    node_view.hpp
    reasoning.cpp
    reasoning.hpp
    stopwatch.cpp
    stopwatch.hpp
    unification.cpp
    unification.hpp
    platform_utils.cpp
    platform_utils.hpp
    string_utils.cpp
    string_utils.hpp
    # zelph::console
    interactive.cpp
    interactive.hpp
    read_async.cpp
    read_async.hpp
    thread_pool.hpp
    wikidata.cpp
    wikidata.hpp
    # boost/serialization. We compile it ourselves. The configurations are (Mac, Linux, Windows) x (CMake, go). In addition we need to match the boost build with our build (e. g. regarding use of C++14).
    ../boost/serialization/src/archive_exception.cpp
    ../boost/serialization/src/basic_archive.cpp
    ../boost/serialization/src/binary_iarchive.cpp
    ../boost/serialization/src/binary_oarchive.cpp
    ../boost/serialization/src/basic_iarchive.cpp
    ../boost/serialization/src/basic_iserializer.cpp
    ../boost/serialization/src/basic_oarchive.cpp
    ../boost/serialization/src/basic_oserializer.cpp
    ../boost/serialization/src/basic_serializer_map.cpp
    ../boost/serialization/src/basic_text_iprimitive.cpp
    ../boost/serialization/src/basic_text_oprimitive.cpp
    ../boost/serialization/src/codecvt_null.cpp
    ../boost/serialization/src/extended_type_info.cpp
    ../boost/serialization/src/extended_type_info_typeid.cpp
    ../boost/serialization/src/text_iarchive.cpp
    ../boost/serialization/src/text_oarchive.cpp
)

target_link_libraries(zelph PRIVATE unordered_dense::unordered_dense)

# Enable IPO/LTO if available
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
if (IPO_SUPPORTED)
    message(STATUS "IPO/LTO enabled")
    set_property(TARGET zelph PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(STATUS "IPO/LTO not supported: ${IPO_OUTPUT}")
endif ()

generate_export_header(zelph)
target_include_directories(zelph PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE .)
add_definitions(-DBOOST_ALL_NO_LIB) # disable boost auto linking under MSVC
include_directories(
    "../boost/algorithm/include"
    "../boost/archive/include"
    "../boost/assert/include"
    "../boost/bimap/include"
    "../boost/bind/include"
    "../boost/concept_check/include"
    "../boost/config_boost/include"
    "../boost/container/include"
    "../boost/container_hash/include"
    "../boost/core/include"
    "../boost/describe/include"
    "../boost/detail/include"
    "../boost/foreach/include"
    "../boost/function/include"
    "../boost/integer/include"
    "../boost/io/include"
    "../boost/iterator/include"
    "../boost/locale/include"
    "../boost/move/include"
    "../boost/mp11/include"
    "../boost/mpl/include"
    "../boost/multi_index/include"
    "../boost/predef/include"
    "../boost/preprocessor/include"
    "../boost/range/include"
    "../boost/serialization/include"
    "../boost/smart_ptr/include"
    "../boost/static_assert/include"
    "../boost/throw_exception/include"
    "../boost/tokenizer/include"
    "../boost/tuple/include"
    "../boost/type_index/include"
    "../boost/type_traits/include"
    "../boost/utility/include"
)

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set_target_properties(zelph PROPERTIES MACOSX_RPATH ON)
endif ()

option(ENABLE_PGO "Enable Profile Guided Optimization" OFF)
if (ENABLE_PGO)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Instrumentation für PGO
        if (NOT PGO_GENERATE)
            # Normal build with PGO data
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use")
            message(STATUS "Using PGO data for optimization")
        else ()
            # Build for generating PGO data
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-generate")
            message(STATUS "Building with instrumentation for PGO data generation")
        endif ()
    endif ()
endif ()
