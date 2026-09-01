#!/usr/bin/env nu
clang-format -i ...(git ls-files -- '*.cpp' '*.hpp' '*.h' | lines)
