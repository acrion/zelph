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
    test_capi.cpp
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
    test_contradiction_records.cpp
    test_ontology.cpp
    test_path_conditions.cpp
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
    test_syntax_errors.cpp
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

# Two checks that hold the build to something it declares about itself, rather
# than to what the machine it was built on happened to offer. Both are wired as
# tests, so the step that already runs ctest carries them on every platform.
find_package(Python3 COMPONENTS Interpreter QUIET)

function(zelph_add_check_test name script arguments expect_code)
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
             -DPYTHON=${Python3_EXECUTABLE}
             -DSCRIPT=${script}
             "-DARGS=${arguments}"
             -DEXPECT_CODE=${expect_code}
             "-DEXPECT_OUTPUT=${ARGN}"
             -P ${CMAKE_CURRENT_SOURCE_DIR}/run_check.cmake)
endfunction()

# --- the instruction set the build may use -----------------------------------

if (ZELPH_TARGET_IS_X86_64 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    find_program(ZELPH_OBJDUMP NAMES objdump llvm-objdump gobjdump)

    if (NOT Python3_Interpreter_FOUND OR NOT ZELPH_OBJDUMP)
        # Rather than vanish without a trace. A check that silently does not
        # run reads exactly like a check that passed.
        message(WARNING
                "Not checking the instruction set floor of the build: it needs python3 and "
                "objdump, and this machine has "
                "python3=${Python3_Interpreter_FOUND} objdump=${ZELPH_OBJDUMP}.")
    else ()
        set(ZELPH_ARCH_CHECK "${CMAKE_SOURCE_DIR}/tools/check_arch_baseline.py")

        include(CheckCXXCompilerFlag)
        string(MAKE_C_IDENTIFIER "ZELPH_SUPPORTS_MARCH_${ZELPH_X86_BASELINE}" ZELPH_TEST_BASELINE_VAR)
        check_cxx_compiler_flag("-march=${ZELPH_X86_BASELINE}" ${ZELPH_TEST_BASELINE_VAR})
        check_cxx_compiler_flag("-mavx512f" ZELPH_SUPPORTS_MAVX512F)

        function(zelph_add_arch_probe name source flag)
            add_library(${name} STATIC ${source})
            # The probe needs to reach the disassembler in machine code, so
            # link time optimization stays off here whatever the build sets
            # globally.
            set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
            target_compile_options(${name} PRIVATE -O3 -fno-lto ${flag})
        endfunction()

        if (${ZELPH_TEST_BASELINE_VAR})
            zelph_add_arch_probe(zelph_arch_probe_baseline arch_probe_baseline.cpp
                                 "-march=${ZELPH_X86_BASELINE}")
            zelph_add_check_test(arch_baseline_accepts_the_floor "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_arch_probe_baseline> --floor ${ZELPH_X86_BASELINE}" 0)
        endif ()

        if (ZELPH_SUPPORTS_MAVX512F)
            zelph_add_arch_probe(zelph_arch_probe_avx512 arch_probe_avx512.cpp "-mavx512f")
            zelph_add_check_test(arch_baseline_refuses_avx512 "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_arch_probe_avx512> --floor ${ZELPH_X86_BASELINE}" 1
                    "above ${ZELPH_X86_BASELINE}")
        endif ()

        # The one that guards the release. The two probes above only establish
        # that the check can tell the two cases apart.
        if (NOT ZELPH_NATIVE_ARCH)
            zelph_add_check_test(arch_baseline_of_the_library "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_lib> --floor ${ZELPH_X86_BASELINE}" 0)
        endif ()
    endif ()
endif ()

# --- the tests that the packages run when a user installs zelph --------------

set(ZELPH_INSTALL_TIER_BUDGET_SECONDS "180" CACHE STRING
    "Seconds the tests outside the slow suite may take, measured where the build runs")

if (Python3_Interpreter_FOUND)
    set(ZELPH_TIER_CHECK "${CMAKE_SOURCE_DIR}/tools/check_install_tier.py")
    set(ZELPH_TIER_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/install_tier_fixtures")

    # What a user actually waits for. It runs the selection the packages run and
    # holds it to its own rules, so a case that grows expensive is found here and
    # not on somebody's machine during an install.
    zelph_add_check_test(install_tier "${ZELPH_TIER_CHECK}"
            "--run $<TARGET_FILE:zelph_tests> --budget ${ZELPH_INSTALL_TIER_BUDGET_SECONDS}" 0)

    # Recorded listings instead of the binary, so these three exercise the rules
    # themselves and stay fast. A selection that no longer refuses what it is
    # meant to refuse is worse than no selection at all.
    zelph_add_check_test(install_tier_accepts_a_healthy_selection "${ZELPH_TIER_CHECK}"
            "--from-listing ${ZELPH_TIER_FIXTURES}/healthy.txt --budget 100" 0)
    zelph_add_check_test(install_tier_refuses_a_dominating_case "${ZELPH_TIER_CHECK}"
            "--from-listing ${ZELPH_TIER_FIXTURES}/dominated.txt --budget 100" 1
            "swallows the selection.* above the allowed")
    zelph_add_check_test(install_tier_refuses_creep "${ZELPH_TIER_CHECK}"
            "--from-listing ${ZELPH_TIER_FIXTURES}/creep.txt --budget 100" 1
            "above the declared budget")

    # Ties the check to a process whose output no strict decoder accepts, which
    # is how the Windows build found it. The check must read such output, not
    # fall over it.
    add_executable(zelph_noisy_listing_probe noisy_listing_probe.cpp)
    zelph_add_check_test(install_tier_survives_undecodable_output "${ZELPH_TIER_CHECK}"
            "--run $<TARGET_FILE:zelph_noisy_listing_probe> --budget 100" 0)
else ()
    # Rather than skip. The tests the packages run at install time are the ones
    # nobody watches, so their guard must not be the first thing to disappear.
    message(WARNING "Not checking what the packages run at install time: python3 was not found.")
endif ()
