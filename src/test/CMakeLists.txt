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
    test_annotations.cpp
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
    test_janet_names.cpp
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
    test_opaque.cpp
    test_path_conditions.cpp
    test_output.cpp
    test_paper_scripts.cpp
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
    test_recipe_ids.cpp
    test_rule_identity.cpp
    test_rule_patterns.cpp
    test_result_query.cpp
    test_save_memory.cpp
    test_script_cluster.cpp
    test_script_query.cpp
    test_script_run.cpp
    test_script_runner.cpp
    test_script_session.cpp
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
    test_versions.cpp
    test_wikidata_constraints.cpp
    test_wikidata_export.cpp
    test_wikidata_import.cpp
    test_wikidata_qualifiers.cpp
)
target_link_libraries(zelph_tests PRIVATE zelph_lib doctest_with_main unordered_dense::unordered_dense)

# Networks saved by prior engine versions, which the tests load as a user
# would. They are compiled into the test binary instead of being loaded from
# the source tree, ensuring those cases run wherever the binary is executed,
# also where a package installs it.
file(GLOB ZELPH_OLDER_NETWORKS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/older_networks/*.bin)
set(ZELPH_OLDER_NETWORKS_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/older_networks.cpp)
add_custom_command(
    OUTPUT ${ZELPH_OLDER_NETWORKS_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/older_networks
            -DOUTPUT=${ZELPH_OLDER_NETWORKS_SOURCE} -DFUNCTION=older_networks
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    DEPENDS ${ZELPH_OLDER_NETWORKS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    COMMENT "Embedding the test networks of an older engine"
    VERBATIM)
target_sources(zelph_tests PRIVATE ${ZELPH_OLDER_NETWORKS_SOURCE})

# Where test_paper_scripts.cpp looks. An absolute path into the SOURCE tree,
# because the scripts are development material and are not installed anywhere:
# the cases that read them are all in the "slow" suite, which the packages
# exclude, so this path is only ever resolved where the source is.
target_compile_definitions(zelph_tests PRIVATE
    ZELPH_PAPER_SCRIPT_DIR="${CMAKE_SOURCE_DIR}/dev_scripts/paper")

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

        find_program(ZELPH_LLVM_OBJDUMP NAMES llvm-objdump)

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

            # rdrand is above x86-64-v3 and keeps the legacy encoding: the
            # check identifies it via its name, and llvm-objdump spells the
            # name with an operand size suffix (rdrandl).
            if (ZELPH_X86_BASELINE STREQUAL "x86-64-v3")
                zelph_add_arch_probe(zelph_arch_probe_legacy_extension arch_probe_legacy_extension.cpp
                                     "-march=${ZELPH_X86_BASELINE}")
                zelph_add_check_test(arch_baseline_refuses_a_legacy_extension "${ZELPH_ARCH_CHECK}"
                        "$<TARGET_FILE:zelph_arch_probe_legacy_extension> --floor ${ZELPH_X86_BASELINE}" 1
                        "rdrand is above the floor")
                if (ZELPH_LLVM_OBJDUMP)
                    zelph_add_check_test(arch_baseline_refuses_a_legacy_extension_read_by_llvm_objdump "${ZELPH_ARCH_CHECK}"
                            "$<TARGET_FILE:zelph_arch_probe_legacy_extension> --floor ${ZELPH_X86_BASELINE} --objdump ${ZELPH_LLVM_OBJDUMP}" 1
                            "rdrandl is above the floor")
                endif ()
            endif ()
        endif ()

        if (ZELPH_SUPPORTS_MAVX512F)
            zelph_add_arch_probe(zelph_arch_probe_avx512 arch_probe_avx512.cpp "-mavx512f")
            zelph_add_check_test(arch_baseline_refuses_avx512 "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_arch_probe_avx512> --floor ${ZELPH_X86_BASELINE}" 1
                    "above ${ZELPH_X86_BASELINE}")

            # llvm-objdump prints its lines with an alternate format, and
            # the check once read none of them: the AVX-512 probe passed.
            if (ZELPH_LLVM_OBJDUMP)
                zelph_add_check_test(arch_baseline_refuses_avx512_read_by_llvm_objdump "${ZELPH_ARCH_CHECK}"
                        "$<TARGET_FILE:zelph_arch_probe_avx512> --floor ${ZELPH_X86_BASELINE} --objdump ${ZELPH_LLVM_OBJDUMP}" 1
                        "above ${ZELPH_X86_BASELINE}")
            endif ()
        endif ()

        # Data within the code section, the shape of MSVC's jump tables, caused
        # the Windows test binary to fail the check due to two (bad) sequences.
        # The check identifies them as data solely after its disassembler has
        # decoded an EVEX sample, a step that GNU objdump performs yet
        # llvm-objdump, without access to raw input, cannot show.
        execute_process(COMMAND "${ZELPH_OBJDUMP}" --version OUTPUT_VARIABLE ZELPH_OBJDUMP_VERSION ERROR_QUIET)
        if (${ZELPH_TEST_BASELINE_VAR} AND ZELPH_OBJDUMP_VERSION MATCHES "GNU objdump")
            zelph_add_arch_probe(zelph_arch_probe_code_section_data arch_probe_code_section_data.cpp
                                 "-march=${ZELPH_X86_BASELINE}")
            zelph_add_check_test(arch_baseline_takes_undecodable_bytes_for_data "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_arch_probe_code_section_data> --floor ${ZELPH_X86_BASELINE}" 0
                    "taken for data")

            # Under --strict, the same bytes are read in the mode for a build
            # that keeps data out of its code section, just as the Windows
            # build does by placing jump tables in .rdata: an undecodable
            # sequence there is no table entry, and the check includes it in
            # its count.
            zelph_add_check_test(arch_baseline_refuses_undecodable_bytes_when_strict "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_arch_probe_code_section_data> --floor ${ZELPH_X86_BASELINE} --strict" 1
                    "EVEX encoded")

            # The identical bytes, read by a disassembler that has not shown
            # it knows EVEX, where (bad) could be an instruction too new for
            # it. This one refuses the sample just as llvm-objdump does, and
            # otherwise responds in the manner of objdump.
            file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/objdump_without_sample.sh" CONTENT [[#!/bin/sh
for argument in "$@"; do
    [ "$argument" = "binary" ] && exit 1
done
exec "@ZELPH_OBJDUMP@" "$@"
]] @ONLY)
            file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/objdump_without_sample.sh"
                 PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
            zelph_add_check_test(arch_baseline_counts_undecodable_bytes_for_an_unproven_disassembler "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_arch_probe_code_section_data> --floor ${ZELPH_X86_BASELINE} --objdump ${CMAKE_CURRENT_BINARY_DIR}/objdump_without_sample.sh" 1
                    "EVEX encoded")
        endif ()

        # The one that guards the release. The two probes above only establish
        # that the check can tell the two cases apart.
        if (NOT ZELPH_NATIVE_ARCH)
            zelph_add_check_test(arch_baseline_of_the_library "${ZELPH_ARCH_CHECK}"
                    "$<TARGET_FILE:zelph_lib> --floor ${ZELPH_X86_BASELINE}" 0)

            # The app binary as well, not only the library. mimalloc is linked
            # into the app and nowhere else, and it carries its own architecture
            # flags, so the one place a dependency could raise the floor is the
            # place the library check does not reach.
            if (TARGET zelph)
                zelph_add_check_test(arch_baseline_of_the_app "${ZELPH_ARCH_CHECK}"
                        "$<TARGET_FILE:zelph> --floor ${ZELPH_X86_BASELINE}" 0)
            endif ()
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

    # --- the status the binary reports ---------------------------------------
    #
    # The doctest suite pins the flag the library keeps; only a real process
    # can be asked what it told the shell, and that is the half a caller
    # actually depends on. It stayed 0 through every failure there is until
    # 1.0.1, which is how a script with an unresolved import passed for a
    # working one for six weeks.
    if (TARGET zelph)
        zelph_add_check_test(exit_status_of_the_binary "${CMAKE_SOURCE_DIR}/tools/check_exit_status.py"
                "$<TARGET_FILE:zelph>" 0)
    endif ()

    # --- what the REPL prints around an input ----------------------------------
    #
    # The library determines via its .timer setting whether a timing line is
    # added to an input, and the doctest suite enforces this rule; this pins
    # that the binary asks after every input. The continuation prompt is
    # displayed exclusively when standard input is connected to a terminal,
    # which only a pseudo-terminal can show, and Python offers none on Windows.
    if (TARGET zelph)
        zelph_add_check_test(timer_of_the_binary "${CMAKE_SOURCE_DIR}/tools/check_timer.py"
                "$<TARGET_FILE:zelph>" 0)
        if (NOT WIN32)
            zelph_add_check_test(continuation_prompt_on_a_terminal "${CMAKE_SOURCE_DIR}/tools/check_continuation_prompt.py"
                    "$<TARGET_FILE:zelph>" 0)
        endif ()
    endif ()

    # --- the version a citation names ------------------------------------------
    #
    # CITATION.cff mirrors the version specified by project(), with no
    # additional linkage between the two. PROJECT_VERSION represents the value
    # reported by the binary (zelph_version.hpp.in).
    zelph_add_check_test(citation_names_the_project_version "${CMAKE_SOURCE_DIR}/tools/check_citation_version.py"
            "${CMAKE_SOURCE_DIR}/CITATION.cff ${PROJECT_VERSION}" 0)

    # --- a Janet program that makes Janet collect ----------------------------
    #
    # Janet 1.42.1 frees the table storing zelph's :err binding during its
    # first garbage collection, and the process died once the event loop read
    # it (src/lib/patch_janet_gc.cmake). Janet 1.41.2 also ran a Janet program
    # started from within another one, as (zelph/import ...) does, on a fiber
    # that a collection might free while active, and two of the cases hold the
    # build to a release that roots it. Since a crash ends the process it
    # encounters, a separate process is invoked instead of the doctest suite.
    if (TARGET zelph)
        zelph_add_check_test(janet_programs_survive_garbage_collection "${CMAKE_SOURCE_DIR}/tools/check_janet_gc.py"
                "$<TARGET_FILE:zelph>" 0)
    endif ()

    # --- a subset cut out of a Wikidata dump ---------------------------------
    #
    # The script `dev_scripts/wikidata-subset.py` builds a network from a dump
    # based on criteria that are data, and nothing else ran it. The check
    # operation generates a dump of its own, extracts a subset from it, imports
    # the subset with the binary of this build, and then queries the network to
    # verify whether the criteria deliver as expected.
    if (TARGET zelph)
        zelph_add_check_test(wikidata_subset_holds_what_its_criteria_keep "${CMAKE_SOURCE_DIR}/tools/check_wikidata_subset.py"
                "$<TARGET_FILE:zelph> ${CMAKE_SOURCE_DIR}/dev_scripts/wikidata-subset.py" 0)
    endif ()

    # --- the paper's timing table --------------------------------------------
    #
    # s9_timings.py names stdlib modules, similar to the scripts
    # test_paper_scripts.cpp runs, yet nothing ran it. Its import row read the
    # REPL's statement timer, which only outputs values starting at 10 ms,
    # causing the script to abort once the import duration fell below this
    # threshold, thereby halting run_all.sh as well. The initial case runs the
    # script's doctests, which pin the mapping between reports and rows and how
    # import time is read. The second case constructs the entire table in one go
    # using the binary from this build and requires the import row's label with a
    # time specified in tenths of a millisecond, a value exclusively available
    # through the clock read within the session.
    set(ZELPH_PAPER_TIMINGS "${CMAKE_SOURCE_DIR}/dev_scripts/paper/s9_timings.py")
    zelph_add_check_test(paper_timings_select_the_claimed_reports "${ZELPH_PAPER_TIMINGS}" "--self-test" 0)
    if (TARGET zelph)
        zelph_add_check_test(paper_timing_table_is_complete "${ZELPH_PAPER_TIMINGS}" "$<TARGET_FILE:zelph> 1" 0
                "inference deferred[)] [|] n/a [|] [0-9]+[.][0-9] ms")
    endif ()

    # --- the allocator the binary names ----------------------------------------
    #
    # Only the app connects to mimalloc, meaning only the app can say which
    # one it runs on; test_versions.cpp covers the remaining part, a binary that
    # links no mimalloc.
    if (TARGET zelph AND TARGET zelph_mimalloc_override)
        get_target_property(ZELPH_LINKED_MIMALLOC zelph_mimalloc_override ZELPH_MIMALLOC_VERSION)
        zelph_add_check_test(version_banner_names_the_linked_mimalloc "${CMAKE_SOURCE_DIR}/tools/check_version_banner.py"
                "$<TARGET_FILE:zelph> --mimalloc ${ZELPH_LINKED_MIMALLOC}" 0)
    endif ()
else ()
    # Rather than skip. The tests the packages run at install time are the ones
    # nobody watches, so their guard must not be the first thing to disappear.
    message(WARNING "Not checking what the packages run at install time: python3 was not found.")
endif ()
