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

# WebAssembly playground: links the zelph engine into a single ES module
# (zelph.mjs + zelph.wasm) usable under Node (M1) and in the browser (M2+).

add_executable(zelph_playground playground.cpp)

target_link_libraries(zelph_playground PRIVATE project_options zelph_lib)

set_target_properties(zelph_playground PROPERTIES
    OUTPUT_NAME "zelph"
    SUFFIX ".mjs"
)

target_link_options(zelph_playground PRIVATE
    "$<$<CONFIG:Release>:-O3>"
    --no-entry                    # library-style module, no main()
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createZelphModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8388608          # parser/reasoner/Janet recursion: be generous
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,FS

    # emcc 6.x can grow the heap via resizable ArrayBuffers
    # (GROWABLE_ARRAYBUFFERS), but Chrome's TextDecoder rejects views on
    # resizable buffers, which breaks the --embed-file loader at startup.
    # Force the classic growth mechanism (replace buffer + recreate views).
    -sGROWABLE_ARRAYBUFFERS=0

    # Package the zelph standard library directly into the module (MEMFS at
    # /stdlib). Unlike --preload-file this produces no separate zelph.data
    # that would need locateFile plumbing in every embedder; the stdlib is
    # tiny, so embedding is the simpler and more robust choice.
    "SHELL:--embed-file ${CMAKE_SOURCE_DIR}/stdlib@/stdlib"
)

# Relink when any stdlib script changes; the embedded copy would otherwise
# go stale silently.
file(GLOB_RECURSE ZELPH_STDLIB_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/stdlib/*)
set_target_properties(zelph_playground PROPERTIES LINK_DEPENDS "${ZELPH_STDLIB_FILES}")

# Mirror the playground web assets next to zelph.mjs on every build. (A
# POST_BUILD command would only run when the wasm target itself relinks,
# so pure web-asset edits would silently not be picked up.)
add_custom_target(zelph_playground_web ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/web
            $<TARGET_FILE_DIR:zelph_playground>
    COMMENT "Mirroring playground web assets next to zelph.mjs")
add_dependencies(zelph_playground_web zelph_playground)
