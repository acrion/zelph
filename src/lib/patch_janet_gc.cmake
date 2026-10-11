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

# Makes Janet's garbage collector mark janet_vm.top_dyns, the table
# containing dynamic bindings set outside any fibre, by inserting the
# marking logic that Janet's master branch has included since commit b437ae3
# (referencing issue #1888) into janet_collect within src/core/gc.c. Janet
# 1.41.2 fails to mark this table, resulting in its premature release during
# the first collection. zelph initializes :err within this table once at
# start-up, and Janet's event loop reads the table when generating a stack
# trace for a finished task (janet_stacktrace_ext): after a collection, that
# read accessed memory that had already been freed, and a Janet program that
# allocated sufficient resources to provoke a collection caused the process
# to terminate with SIGSEGV.
#
# Remove this script and its call in CMakeLists.txt once the pinned Janet
# release automatically marks top_dyns. Until then, the script serves as a
# no-op in a tree that already holds the marking, and it causes
# configuration to fail when the code it inserts after shifts position,
# preventing a Janet bump from silently dropping the fix.
#
# Usage: cmake -DJANET_SOURCE_DIR=<janet source tree> -P patch_janet_gc.cmake

if (NOT DEFINED JANET_SOURCE_DIR)
    message(FATAL_ERROR "patch_janet_gc.cmake needs -DJANET_SOURCE_DIR=")
endif ()

set(gc_file "${JANET_SOURCE_DIR}/src/core/gc.c")
if (NOT EXISTS "${gc_file}")
    message(FATAL_ERROR "patch_janet_gc.cmake: ${gc_file} does not exist")
endif ()

file(READ "${gc_file}" gc_source)

string(FIND "${gc_source}" "janet_mark(janet_wrap_table(janet_vm.top_dyns))" already_marked)
if (NOT already_marked EQUAL -1)
    return()
endif ()

# The end of the event loop's marking within janet_collect, after which the
# marking is inserted, following the same sequence as in Janet's master
# branch. A checkout configured with CRLF line endings is also compatible,
# and the inserted lines take the line ending style found in the file.
string(REGEX MATCH "\n    janet_ev_mark\\(\\);\r?\n#endif\r?\n" anchor "${gc_source}")
set(anchor_at -1)
set(last_at -1)
if (NOT anchor STREQUAL "")
    string(FIND "${gc_source}" "${anchor}" anchor_at)
    string(FIND "${gc_source}" "${anchor}" last_at REVERSE)
endif ()
if (anchor_at EQUAL -1 OR NOT anchor_at EQUAL last_at)
    message(FATAL_ERROR
            "patch_janet_gc.cmake: the end of janet_ev_mark()'s block in janet_collect is not where it was in "
            "${gc_file} (expected exactly once). If this Janet release marks janet_vm.top_dyns in janet_collect, "
            "remove the patch; otherwise adapt it to the new code.")
endif ()

string(FIND "${anchor}" "\r\n" crlf_at)
if (crlf_at EQUAL -1)
    set(eol "\n")
else ()
    set(eol "\r\n")
endif ()

string(LENGTH "${anchor}" anchor_length)
math(EXPR insert_at "${anchor_at} + ${anchor_length}")
string(SUBSTRING "${gc_source}" 0 ${insert_at} before)
string(SUBSTRING "${gc_source}" ${insert_at} -1 after)

file(WRITE "${gc_file}"
     "${before}"
     "    if (janet_vm.top_dyns) {${eol}"
     "        janet_mark(janet_wrap_table(janet_vm.top_dyns));${eol}"
     "    }${eol}"
     "${after}")

message(STATUS "Patched ${gc_file}: janet_collect marks janet_vm.top_dyns")
