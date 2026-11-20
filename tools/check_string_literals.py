#!/usr/bin/env python3
"""Hold every string literal of the sources to the length Visual Studio 2022 takes.

MSVC up to 19.44, the compiler of Visual Studio 2022, takes no more than 16,380
bytes in one string literal: the limit applies to each literal as it is
written, ahead of the concatenation of adjacent literals, and a longer one
stops the build with error C2026 ("string too big, trailing characters
truncated"). No other compiler the project is built with has such a limit, so
a literal that outgrows it builds everywhere else and fails for whoever uses
that compiler. The remedy is to write the text as adjacent literals, each
within the limit, which every compiler joins into the same string.

The check reads the sources as a compiler's lexer does, as far as it must to
find the literals: it skips comments and character literals, takes a raw
string literal from its opening to its closing delimiter whatever stands
between them, and measures every literal on its own, in the bytes of its
spelling between the quotes. An escape sequence counts as it is spelled, and a
literal of wide characters (``L``, ``u``, ``U``) counts two or four bytes for
every byte of its spelling, so the measure is never below the length the
compiler sees.

Exit codes: 0 every literal holds the limit, 1 one of them does not, 2 the
question could not be put.
"""

import argparse
import contextlib
import io
import os
import sys
import tempfile

# Microsoft documents the figure with error C2026: before concatenation of
# adjacent strings, a string must not exceed 16380 single-byte characters.
LIMIT = 16380

SUFFIXES = (".cpp", ".hpp", ".h", ".cc", ".c")

# What can appear immediately before the quote of a string literal.
ENCODINGS = ("", "u8", "u", "U", "L")

# The minimum number of bytes a character in a literal occupies, determined by
# the literal's prefix: MSVC counts a wide literal in bytes too, meaning a
# UTF-16 literal reaches the limit with half as many characters.
WIDTHS = {"": 1, "u8": 1, "u": 2, "L": 2, "U": 4}


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def literals(text):
    """Yield (line, bytes) for every string literal of a C++ source text."""
    position = 0
    line = 1
    size = len(text)
    prefix = ""

    def advance(to):
        nonlocal position, line
        line += text.count("\n", position, to)
        position = to

    while position < size:
        char = text[position]

        if text.startswith("//", position):
            end = text.find("\n", position)
            advance(size if end < 0 else end)
        elif text.startswith("/*", position):
            end = text.find("*/", position + 2)
            advance(size if end < 0 else end + 2)
        elif char.isalnum() or char == "_":
            # A name or a number, read as a single word. Within a number,
            # an apostrophe separates digits and does not initiate a character
            # literal.
            number = char.isdigit()
            end = position + 1
            while end < size and (text[end].isalnum() or text[end] == "_" or
                                  (number and text[end] in ".'" and end + 1 < size and text[end + 1].isalnum())):
                end += 1
            word = text[position:end]
            if end < size and text[end] == '"' and word.endswith("R") and word[:-1] in ENCODINGS:
                # R"delimiter( ... )delimiter"
                opening = text.find("(", end)
                if opening < 0:
                    advance(size)
                    continue
                closing = ")" + text[end + 1:opening] + '"'
                close = text.find(closing, opening + 1)
                start_line = line + text.count("\n", position, opening)
                if close < 0:
                    advance(size)
                    continue
                yield start_line, len(text[opening + 1:close].encode("utf-8")) * WIDTHS[word[:-1]]
                advance(close + len(closing))
            else:
                # The width of the literal that this word serves as the
                # prefix for, if a quote comes immediately after it.
                prefix = word if word in ENCODINGS and end < size and text[end] == '"' else ""
                # An encoding prefix leaves the quote behind it to the branches
                # beneath, which read it just as they would one lacking a
                # prefix.
                advance(end)
        elif char == '"' or char == "'":
            end = position + 1
            while end < size and text[end] != char and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            if char == '"':
                yield line, len(text[position + 1:end].encode("utf-8")) * WIDTHS[prefix]
            prefix = ""
            advance(min(end + 1, size))
        else:
            prefix = ""
            advance(position + 1)


def sources(roots):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        if not os.path.isdir(root):
            fail(f"no such file or directory: {root}")
        for directory, _, names in sorted(os.walk(root)):
            for name in sorted(names):
                if name.endswith(SUFFIXES):
                    yield os.path.join(directory, name)


def self_test():
    """Hold the reader to the cases that decide what it measures."""
    long_text = "x" * (LIMIT + 1)
    half = "x" * (LIMIT // 2 + 1)
    raw_text = 'say "hi" // not a comment ) still'
    cases = [
        # (name, source, the lengths of the literals it holds, in order)
        ("an ordinary literal", 'auto a = "abc";', [3]),
        ("an escaped quote stays inside", r'auto a = "a\"b";', [4]),
        ("a raw literal with quotes, a comment mark and a parenthesis",
         f'auto a = R"zph({raw_text})zph";', [len(raw_text)]),
        ("a raw literal over several lines", 'auto a = R"(one\ntwo)";', [7]),
        ("a raw literal counts bytes, not characters", 'auto a = R"(≈)";', [3]),
        ("an ordinary literal counts bytes, not characters", 'auto a = "≈";', [3]),
        ("a literal that is not closed ends with its line", 'auto a = "abc\nauto b = "de";', [3, 2]),
        ("prefixes of one byte", 'auto a = u8"ab"; auto b = u8R"x(abc)x";', [2, 3]),
        ("a wide literal counts two bytes a character, or four", 'auto a = L"ab"; auto b = u"abc"; auto c = U"ab";', [4, 6, 8]),
        ("a wide raw literal as well", 'auto a = LR"x(abc)x"; auto b = uR"(ab)"; auto c = UR"(a)";', [6, 4, 4]),
        ("a prefix counts only directly ahead of the quote", 'auto L = 1; auto a = "ab"; f(U, "abc");', [2, 3]),
        ("adjacent literals are measured one by one", 'auto a = "ab" "cde"\n    R"(f)";', [2, 3, 1]),
        ("a comment holds no literal", '// "abc"\n/* "abcd" */ auto a = "ab";', [2]),
        ("a character literal holds no literal", "char a = '\"'; char b = '\\''; auto c = \"ab\";", [2]),
        ("a digit separator opens no character literal", "auto a = 1'000; auto b = \"ab\";", [2]),
        ("a name that ends in R is no raw prefix", 'auto a = VAR"ab";', [2]),
        ("a literal above the limit", f'auto a = "{long_text}";', [LIMIT + 1]),
        ("a raw literal above the limit", f'auto a = R"zph({long_text})zph";', [LIMIT + 1]),
        ("two adjacent halves stay below the limit", f'auto a = "{half}" "{half}";', [len(half), len(half)]),
        ("a comment above the limit", f"// {long_text}\nauto a = \"ab\";", [2]),
    ]

    failures = []
    for name, source, expected in cases:
        found = [length for _, length in literals(source)]
        if found != expected:
            failures.append(f"{name}: expected {expected}, read {found}")

    # The line at which a literal is reported is the line on which it begins.
    lines = [line for line, _ in literals('\n\nauto a = "ab";\nauto b = R"(\n\n)";\nauto c = "d";')]
    if lines != [3, 4, 7]:
        failures.append(f"lines: expected [3, 4, 7], read {lines}")

    # The verdict on files: a directory whose sources hold the limit, one of
    # them bearing a literal of exactly that length, a directory with a literal
    # above the limit, and a file of a different kind, which is not read when
    # its directory is named, but is read when it is named itself.
    with tempfile.TemporaryDirectory(prefix="zelph_literals_") as directory:
        good = os.path.join(directory, "good")
        bad = os.path.join(directory, "bad")
        os.makedirs(os.path.join(good, "nested"))
        os.makedirs(bad)
        with open(os.path.join(good, "nested", "fits.cpp"), "w", encoding="utf-8") as handle:
            handle.write(f'auto a = "{half}" "{half}";\n')
        with open(os.path.join(good, "exact.cc"), "w", encoding="utf-8") as handle:
            handle.write(f'auto a = "{long_text[:LIMIT]}";\n')
        with open(os.path.join(good, "notes.txt"), "w", encoding="utf-8") as handle:
            handle.write(f'auto a = "{long_text}";\n')
        with open(os.path.join(bad, "too_long.hpp"), "w", encoding="utf-8") as handle:
            handle.write(f'\n\nauto a = "{long_text}";\n')

        verdicts = []
        for roots in ([good], [bad], [good, bad], [os.path.join(good, "notes.txt")]):
            said = io.StringIO()
            with contextlib.redirect_stdout(said), contextlib.redirect_stderr(said):
                verdicts.append((run(roots), said.getvalue()))
        if [code for code, _ in verdicts] != [0, 1, 1, 1]:
            failures.append(f"verdicts: expected [0, 1, 1, 1], got {[code for code, _ in verdicts]}")
        if f"2 files: no string literal is longer than {LIMIT} bytes (the longest has {LIMIT}," not in verdicts[0][1]:
            failures.append(f"verdicts: the directory that holds the limit was reported as {verdicts[0][1]!r}")
        if f"too_long.hpp:3: a string literal of {LIMIT + 1} bytes" not in verdicts[1][1]:
            failures.append(f"verdicts: the literal above the limit was reported as {verdicts[1][1]!r}")

    if failures:
        for failure in failures:
            print(f"error: {failure}", file=sys.stderr)
        return 1
    print(f"{len(cases) + 2} cases: the check reads string literals as a compiler does.")
    return 0


def run(roots):
    """Read the sources below `roots` and say what was found; the exit code."""
    files = 0
    longest = (0, "", 0)
    violations = []
    for path in sources(roots):
        try:
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
        except (OSError, UnicodeDecodeError) as exc:
            fail(f"cannot read {path}: {exc}")
        files += 1
        for line, length in literals(text):
            if length > longest[0]:
                longest = (length, path, line)
            if length > LIMIT:
                violations.append(f"{path}:{line}: a string literal of {length} bytes; Visual Studio 2022 takes "
                                  f"{LIMIT} in one literal (error C2026). Write it as adjacent literals.")

    if not files:
        fail("no source file found")
    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1

    print(f"{files} files: no string literal is longer than {LIMIT} bytes "
          f"(the longest has {longest[0]}, {longest[1]}:{longest[2]}).")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("roots", nargs="*", help="source files, or directories whose C and C++ sources are read")
    parser.add_argument("--self-test", action="store_true", help="check the reader itself against fixed cases")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.roots:
        fail("name the sources to read")
    return run(args.roots)


if __name__ == "__main__":
    sys.exit(main())
