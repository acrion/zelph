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

include(GenerateExportHeader)
include(CheckCXXCompilerFlag)
include(FetchContent)
include(CheckIPOSupported)

FetchContent_Declare(
    janet
    GIT_REPOSITORY https://github.com/janet-lang/janet.git
    GIT_TAG v1.40.1
    SOURCE_SUBDIR ""
)

FetchContent_MakeAvailable(janet)

set(OLD_ENV_CC $ENV{CC})
set(OLD_ENV_CXX $ENV{CXX})
set(OLD_ENV_PATH $ENV{PATH})

if (MSVC)
    set(MESON_OPTIONS "--vsenv")

    set(CLEAN_PATH_LIST "")
    file(TO_CMAKE_PATH "$ENV{PATH}" PATH_LIST)
    foreach (P IN LISTS PATH_LIST)
        if (NOT "${P}" MATCHES "Strawberry")
            list(APPEND CLEAN_PATH_LIST "${P}")
        endif ()
    endforeach ()
    list(JOIN CLEAN_PATH_LIST ";" NEW_PATH_STR)
    file(TO_NATIVE_PATH "${NEW_PATH_STR}" NEW_NATIVE_PATH)
    set(ENV{PATH} "${NEW_NATIVE_PATH}")

    set(BUILD_COMMAND meson compile -C build --verbose)
else ()
    set(JANET_LIB_NAME "libjanet.a")
    set(MESON_OPTIONS "")

    set(ENV{CC} "${CMAKE_C_COMPILER}")
    set(ENV{CXX} "${CMAKE_CXX_COMPILER}")

    set(BUILD_COMMAND ninja -C build)
endif ()

if(CMAKE_C_COMPILER_TARGET)
    # Generate a Meson cross file for cross-compilation
    set(MESON_CROSS_FILE "${janet_SOURCE_DIR}/cross_file.ini")
    file(WRITE ${MESON_CROSS_FILE}
"[binaries]\n\
c = ['${CMAKE_C_COMPILER}', '--target=${CMAKE_C_COMPILER_TARGET}']\n\
cpp = ['${CMAKE_CXX_COMPILER}', '--target=${CMAKE_CXX_COMPILER_TARGET}']\n\
ar = 'aarch64-linux-gnu-ar'\n\
strip = 'aarch64-linux-gnu-strip'\n\
\n\
[host_machine]\n\
system = 'linux'\n\
cpu_family = 'aarch64'\n\
cpu = 'aarch64'\n\
endian = 'little'\n"
    )
    set(MESON_CROSS_ARGS "--cross-file" "${MESON_CROSS_FILE}")
else()
    set(MESON_CROSS_ARGS "")
endif()

execute_process(
    COMMAND meson setup build --buildtype=release -Ddynamic_modules=false ${MESON_OPTIONS} ${MESON_CROSS_ARGS}
    WORKING_DIRECTORY ${janet_SOURCE_DIR}
    RESULT_VARIABLE result
)
if (result)
    message(FATAL_ERROR "Janet Meson setup failed: ${result}")
endif ()

execute_process(
    COMMAND ${BUILD_COMMAND}
    WORKING_DIRECTORY ${janet_SOURCE_DIR}
    RESULT_VARIABLE result
)
if (result)
    message(FATAL_ERROR "Janet build failed: ${result}")
endif ()

set(ENV{CC} "${OLD_ENV_CC}")
set(ENV{CXX} "${OLD_ENV_CXX}")
set(ENV{PATH} "${OLD_ENV_PATH}")

if (MSVC)
    file(GLOB JANET_RUNTIME_DLLS "${janet_SOURCE_DIR}/build/janet-*.dll")
    list(LENGTH JANET_RUNTIME_DLLS JANET_RUNTIME_DLL_COUNT)
    if (JANET_RUNTIME_DLL_COUNT EQUAL 0)
        message(FATAL_ERROR "Janet runtime DLL not found in ${janet_SOURCE_DIR}/build")
    endif ()
    list(GET JANET_RUNTIME_DLLS 0 JANET_RUNTIME_DLL)

    add_library(janet_lib SHARED IMPORTED)

    set_target_properties(janet_lib PROPERTIES
        IMPORTED_IMPLIB "${janet_SOURCE_DIR}/build/janet.lib"
        IMPORTED_LOCATION "${JANET_RUNTIME_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${janet_SOURCE_DIR}/src/include;${janet_SOURCE_DIR}/build"
    )
else ()
    add_library(janet_lib STATIC IMPORTED)

    set_target_properties(janet_lib PROPERTIES
        IMPORTED_LOCATION ${janet_SOURCE_DIR}/build/${JANET_LIB_NAME}
        INTERFACE_INCLUDE_DIRECTORIES "${janet_SOURCE_DIR}/src/include;${janet_SOURCE_DIR}/build"
    )
endif ()

target_compile_definitions(janet_lib INTERFACE JANET_BUILD="amalgam")

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
)

set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

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

capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS io/zelph.capnp)

add_library(zelph_lib SHARED
    command_executor.cpp
    command_executor.hpp
    interactive.cpp
    interactive.hpp
    repl_state.hpp
    script_engine.cpp
    script_engine.hpp

    chrono/stopwatch.cpp
    chrono/stopwatch.hpp

    concurrency/thread_pool.hpp

    io/data_manager.hpp
    io/data_manager.cpp
    io/markdown.cpp
    io/markdown.hpp
    io/mermaid.cpp
    io/mermaid.hpp
    io/output.cpp
    io/output.hpp
    io/read_async.cpp
    io/read_async.hpp

    network/adjacency_set.hpp
    network/answer.cpp
    network/answer.hpp
    network/contradiction_error.hpp
    network/fact_structure.hpp
    network/fact_structure_types.hpp
    network/network.hpp
    network/reasoning.cpp
    network/reasoning_deduce.cpp
    network/reasoning_evaluate.cpp
    network/reasoning_pruning.cpp
    network/reasoning.hpp
    network/reasoning_profiler.hpp
    network/unification.cpp
    network/unification.hpp
    network/zelph.cpp
    network/zelph_names.cpp
    network/zelph_maintenance.cpp
    network/zelph.hpp
    network/zelph_impl.hpp

    platform/platform_utils.cpp
    platform/platform_utils.hpp

    string/node_to_string.cpp
    string/node_to_string.hpp
    string/string_utils.cpp
    string/string_utils.hpp

    wikidata/wikidata_text_compressor.hpp
    wikidata/wikidata_token_encoder.hpp
    wikidata/wikidata.cpp
    wikidata/wikidata.hpp

    ${CAPNP_SRCS}
)


target_link_libraries(zelph_lib PUBLIC project_options)

set(ZELPH_OPT_FLAGS "")
check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND ZELPH_OPT_FLAGS "-O3")
    if (COMPILER_SUPPORTS_MARCH_NATIVE)
        list(APPEND ZELPH_OPT_FLAGS "-march=native")
    endif ()

    # CRITICAL: Do NOT use -ffast-math!
    # Janet uses NaN-boxing on 64-bit systems to pack type information and pointers into double-precision floats.
    # -ffast-math enables -ffinite-math-only, which assumes NaNs do not exist. This causes the compiler to
    # optimize away the bitwise checks Janet uses to distinguish objects (NaNs) from numbers, breaking
    # the type system completely and leading to undefined behavior (e.g. strings treated as 0.0).
    list(APPEND ZELPH_OPT_FLAGS "-flto=auto" "-funroll-loops")

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND ZELPH_OPT_FLAGS "-fomit-frame-pointer")
        if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9.0)
            list(APPEND ZELPH_OPT_FLAGS "-fno-semantic-interposition")
        endif ()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        list(APPEND ZELPH_OPT_FLAGS "-fomit-frame-pointer")
    endif ()

elseif (MSVC)
    list(APPEND ZELPH_OPT_FLAGS "/O2" "/Oi" "/Ot" "/GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG")
endif ()

target_compile_options(zelph_lib PRIVATE $<$<CONFIG:Release>:${ZELPH_OPT_FLAGS}>)

target_link_libraries(zelph_lib PRIVATE unordered_dense::unordered_dense CapnProto::capnp bz2 janet_lib)
if (WIN32)
    # ws2_32 and Mswsock are required by Janet
    target_link_libraries(zelph_lib PRIVATE ws2_32 Mswsock)
endif ()

# Suppress warnings from Cap'n Proto includes in zelph_lib
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(zelph_lib PRIVATE -w -fpermissive)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(zelph_lib PRIVATE /w)
endif ()

# Enable IPO/LTO if available
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

target_include_directories(zelph_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zelph>
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
