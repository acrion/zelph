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
    find_program(CLANG clang REQUIRED)
    find_program(CLANGXX clang++ REQUIRED)
    set(CMAKE_C_COMPILER "${CLANG}")
    set(CMAKE_CXX_COMPILER "${CLANGXX}")
    message(STATUS "Using Clang compiler at ${CLANGXX}")
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

    set(OPTIMIZATION_FLAGS "${OPTIMIZATION_FLAGS} -flto=auto")

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
    janet
    GIT_REPOSITORY https://github.com/janet-lang/janet.git
    GIT_TAG v1.40.1
    SOURCE_SUBDIR ""
)

FetchContent_MakeAvailable(janet)

execute_process(
    COMMAND meson setup build --buildtype=release -Ddynamic_modules=false
    WORKING_DIRECTORY ${janet_SOURCE_DIR}
    RESULT_VARIABLE result
)
if (result)
    message(FATAL_ERROR "Janet Meson setup failed: ${result}")
endif ()

execute_process(
    COMMAND ninja -C build
    WORKING_DIRECTORY ${janet_SOURCE_DIR}
    RESULT_VARIABLE result
)
if (result)
    message(FATAL_ERROR "Janet Ninja build failed: ${result}")
endif ()

add_library(janet_lib STATIC IMPORTED)
set_target_properties(janet_lib PROPERTIES
    IMPORTED_LOCATION ${janet_SOURCE_DIR}/build/libjanet.a
    INTERFACE_INCLUDE_DIRECTORIES "${janet_SOURCE_DIR}/src/include;${janet_SOURCE_DIR}/build"
)
target_compile_definitions(janet_lib INTERFACE JANET_BUILD="\"amalgam\"")
if (NOT WIN32)
    target_compile_options(janet_lib INTERFACE -pthread)
endif ()

FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.8.1
)

FetchContent_Declare(
    CapnProto
    GIT_REPOSITORY https://github.com/capnproto/capnproto.git
    GIT_TAG v1.3.0
    CMAKE_ARGS -DBUILD_TESTING=OFF
)

FetchContent_MakeAvailable(unordered_dense CapnProto)

# Suppress all warnings from Cap'n Proto targets (third-party), including LTO
set(CAPNP_TARGETS kj kj-async capnp capnp-json capnpc capnp_tool capnpc_cpp)
foreach (target IN LISTS CAPNP_TARGETS)
    if (TARGET ${target})
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE -w)
            target_link_options(${target} PRIVATE -w)
        elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /w)
            target_link_options(${target} PRIVATE /w)
        endif ()
    endif ()
endforeach ()

FetchContent_Declare(
    bzip2
    GIT_REPOSITORY https://gitlab.com/bzip2/bzip2.git
    GIT_TAG bzip2-1.0.8
)

FetchContent_GetProperties(bzip2)
if (NOT bzip2_POPULATED)
    FetchContent_Populate(bzip2)
    add_library(bz2 STATIC
        ${bzip2_SOURCE_DIR}/blocksort.c
        ${bzip2_SOURCE_DIR}/bzlib.c
        ${bzip2_SOURCE_DIR}/compress.c
        ${bzip2_SOURCE_DIR}/crctable.c
        ${bzip2_SOURCE_DIR}/decompress.c
        ${bzip2_SOURCE_DIR}/huffman.c
        ${bzip2_SOURCE_DIR}/randtable.c
    )
    target_include_directories(bz2 PUBLIC ${bzip2_SOURCE_DIR})
    if (MSVC)
        target_compile_definitions(bz2 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif ()
    set_target_properties(bz2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif ()

#FetchContent_Declare(
#    acrion_cmake
#    GIT_REPOSITORY git@github.com:acrion/cmake.git
#    GIT_TAG main
#)
#FetchContent_MakeAvailable(acrion_cmake)
#
#include(${acrion_cmake_SOURCE_DIR}/set-cpp-version.cmake)

capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS zelph.capnp)

add_library(zelph_lib SHARED
    # zelph::network
    adjacency_set.hpp
    answer.cpp
    answer.hpp
    zelph.cpp
    zelph.hpp
    zelph_impl.hpp
    contradiction_error.hpp
    markdown.cpp
    markdown.hpp
    network.hpp
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
    thread_pool.hpp
    wikidata_text_compressor.hpp
    wikidata_token_encoder.hpp
    # zelph::console
    data_manager.hpp
    data_manager.cpp
    interactive.cpp
    interactive.hpp
    read_async.cpp
    read_async.hpp
    wikidata.cpp
    wikidata.hpp
    ${CAPNP_SRCS}
)

target_link_libraries(zelph_lib PRIVATE unordered_dense::unordered_dense CapnProto::capnp bz2 janet_lib)

# Suppress warnings from Cap'n Proto includes in zelph_lib
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(zelph_lib PRIVATE -w -fpermissive)
    target_link_options(zelph_lib PRIVATE -w)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(zelph_lib PRIVATE /w)
    target_link_options(zelph_lib PRIVATE /w)
endif ()

# Enable IPO/LTO if available
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
if (IPO_SUPPORTED)
    message(STATUS "IPO/LTO enabled")
    set_property(TARGET zelph_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(STATUS "IPO/LTO not supported: ${IPO_OUTPUT}")
endif ()

generate_export_header(zelph_lib
    BASE_NAME ZELPH
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/zelph_export.h
)
target_include_directories(zelph_lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE .)
add_definitions(-DBOOST_ALL_NO_LIB) # disable boost auto linking under MSVC
include_directories(
    "../boost/algorithm/include"       # used in interactive.cpp and zelph.cpp
    "../boost/bimap/include"           # used in interactive.cpp
    "../boost/locale/include"          # used in string_utils.cpp
    "../boost/tokenizer/include"       # used in wikidata.cpp and interactive.cpp

    "../boost/range/include"           # required by e.g. boost/algorithm
    "../boost/function/include"        # required by e.g. boost/algorithm
    "../boost/mpl/include"             # required by e.g. boost/algorithm
    "../boost/container_hash/include"  # required by e.g. boost/bimap
    "../boost/multi_index/include"     # required by e.g. boost/bimap
    "../boost/iterator/include"        # required by e.g. boost/tokenizer
    "../boost/throw_exception/include" # required by e.g. boost/tokenizer

    "../boost/assert/include"          # required by e.g. boost/range
    "../boost/concept_check/include"   # required by e.g. boost/range
    "../boost/preprocessor/include"    # required by e.g. boost/range
    "../boost/type_traits/include"     # required by e.g. boost/range
    "../boost/utility/include"         # required by e.g. boost/range
    "../boost/bind/include"            # required by e.g. boost/function
    "../boost/describe/include"        # required by e.g. boost/container_hash
    "../boost/move/include"            # required by e.g. boost/multi_index
    "../boost/tuple/include"           # required by e.g. boost/multi_index
    "../boost/mp11/include"            # required by e.g. boost/iterator
    "../boost/core/include"            # required by e.g. boost/iterator

    "../boost/static_assert/include"   # required by e.g. boost/type_traits
    "../boost/config_boost/include"    # required by all of boost
)

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set_target_properties(zelph_lib PROPERTIES MACOSX_RPATH ON)
endif ()

set_target_properties(zelph_lib PROPERTIES OUTPUT_NAME zelph)

install(TARGETS zelph_lib
    EXPORT zelph_libTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zelph
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    PATTERN "CMakeLists.txt" EXCLUDE
)

install(FILES ${CAPNP_HDRS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zelph)

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
