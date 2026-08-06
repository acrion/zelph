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

# doctest v2.4.12 declares cmake_minimum_required(VERSION 3.1),
# which newer CMake rejects. Allow it to configure anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.2
    SYSTEM
)
FetchContent_MakeAvailable(doctest)

FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.8.1
    SYSTEM
)

FetchContent_MakeAvailable(unordered_dense)

add_executable(zelph_tests
    test_bin_inspection.cpp
    test_check_fact.cpp
    test_clusters.cpp
    test_core_names.cpp
    test_derivation_export.cpp
    test_derived_rules.cpp
    test_display_scheme.cpp
    test_explain.cpp
    test_fact_cache.cpp
    test_genuine_structure.cpp
    test_hf_cache.cpp
    test_import_guard.cpp
    test_inline_keyword.cpp
    test_integers.cpp
    test_jacobian.cpp
    test_math.cpp
    test_math_identities.cpp
    test_math_syntax.cpp
    test_mermaid.cpp
    test_name_merge.cpp
    test_nand_arithmetic.cpp
    test_neural.cpp
    test_node_display.cpp
    test_numbers.cpp
    test_intervals.cpp
    test_ontology.cpp
    test_output.cpp
    test_partial_anchor.cpp
    test_performance_guard.cpp
    test_polynomial.cpp
    test_pow.cpp
    test_predicate_index.cpp
    test_predicate_slice.cpp
    test_primes.cpp
    test_profiler.cpp
    test_pruning.cpp
    test_reasoning.cpp
    test_rule_identity.cpp
    test_rule_patterns.cpp
    test_result_query.cpp
    test_save_memory.cpp
    test_script_cluster.cpp
    test_script_query.cpp
    test_script_run.cpp
    test_script_runner.cpp
    test_self_predicate.cpp
    test_seminaive.cpp
    test_sets.cpp
    test_sharding.cpp
    test_sparql.cpp
    test_stratified.cpp
    test_symbolic.cpp
    test_symbolic_integers.cpp
    test_topoly.cpp
    test_var_closure.cpp
    test_wikidata_constraints.cpp
    test_wikidata_export.cpp
    test_wikidata_import.cpp
    test_wikidata_qualifiers.cpp
)
target_link_libraries(zelph_tests PRIVATE zelph_lib doctest_with_main unordered_dense::unordered_dense)

# Mirror the standard library next to the test binary. Same rationale as in
# src/app; needed independently so tests work when the app target is not
# built, and so packaged test binaries (Chocolatey, Homebrew) find the
# stdlib next to themselves.
#
# Source-driven for the same reason as in src/app, and here it is a
# CORRECTNESS issue rather than a convenience: as a POST_BUILD step, a
# change to a .zph file alone relinked nothing, so the suite ran against
# the previously mirrored stdlib and reported a stale result. Removing the
# destination first matters for the same reason -- a stale orphan is a
# module the suite can still import.
file(GLOB_RECURSE ZELPH_TEST_STDLIB_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/stdlib/*)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stdlib_mirror.stamp
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:zelph_tests>/stdlib
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/stdlib
            $<TARGET_FILE_DIR:zelph_tests>/stdlib
    COMMAND ${CMAKE_COMMAND} -E touch
            ${CMAKE_CURRENT_BINARY_DIR}/stdlib_mirror.stamp
    DEPENDS ${ZELPH_TEST_STDLIB_SOURCES} $<TARGET_FILE:zelph_tests>
    COMMENT "Mirroring zelph standard library next to the test binary")

add_custom_target(zelph_tests_stdlib_mirror ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/stdlib_mirror.stamp)

# Both mirrors write to CMAKE_RUNTIME_OUTPUT_DIRECTORY/stdlib, i.e. the SAME
# directory whenever the app is built too. Overlapping copies were harmless,
# but overlapping remove-then-copy is not: a parallel build could delete the
# tree the other command is still filling. Serialising the two costs one
# redundant copy and removes the race.
if(TARGET zelph_stdlib_mirror)
    add_dependencies(zelph_tests_stdlib_mirror zelph_stdlib_mirror)
endif()

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
