#!/usr/bin/env bash
# Format every tracked C++ source and header with the repository's .clang-format.
# Equivalent to clang-format.nu beside it, for shells other than nushell.
#
# Run it from anywhere in the working tree; git ls-files is resolved against the
# repository root, so the file list does not depend on the current directory.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
git ls-files -z -- '*.cpp' '*.hpp' '*.h' | xargs -0 --no-run-if-empty clang-format -i
