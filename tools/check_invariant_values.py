#!/usr/bin/env python3
"""Hold the values the measurement page names for the invariant set to the values the suite holds.

The values of the hard semantic invariants in the standalone sequential run of
the Jacobian example live in two files that nothing ties together: a block of
mkdocs/docs/internals/measurement.md, which is what a reader compares a dump
with, and a case of src/test/test_performance_guard.cpp, which holds the run to
them. A change that moves the set brings its new values into the case, because
the case fails otherwise, and the page goes on naming the old ones.

The page's block is the fenced code block after the HTML comment that names
this script. Each of its lines is written as ``.prof`` writes it: the label of
a block, such as ``evaluate:``, where the dump has one, and then ``name=value``
pairs. The case's values are its comparisons of one counter with a number,
``prof_counter(prof, "<section>", "<name>") == <value>``, and of a whole block
with a text, ``prof_block(prof, "<label>") == "<name>=<value> ..."``. A
section that does not end in a colon is a counter of the dump's first line,
which bears no label, and the counter found after it is named without one.

Exit codes: 0 the two agree, 1 they differ, 2 the question could not be put.
With --self-test: 0 every case of the self-test passes, 1 at least one fails.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

# The case whose values the page names.
CASE = "performance guard: the sequential Jacobian workload keeps every hard semantic invariant at its value"

# The HTML comment preceding the page's block names this script.
MARKER = "check_invariant_values.py"

PAIR = re.compile(r"([A-Za-z_][A-Za-z0-9_()]*)=([0-9]+)")

COUNTER = re.compile(r'prof_counter\(\s*\w+\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)\s*==\s*([0-9]+)')

BLOCK = re.compile(r'prof_block\(\s*\w+\s*,\s*"([^"]+)"\s*\)\s*==\s*"([^"]*)"')

# A string literal, which is kept, or a comment, which is dropped: a comment
# contributes no value, and a // within a string does not cut the string short.
LITERAL_OR_COMMENT = re.compile(r'("(?:\\.|[^"\\\n])*")|//[^\n]*|/\*.*?\*/', re.DOTALL)


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def read(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read()
    except (OSError, UnicodeDecodeError) as exc:
        fail(f"cannot read {path}: {exc}")


def add_pairs(text, label, where, values):
    """Add the name=value pairs of `text` to `values`, named under `label`."""
    for token in text.split():
        match = PAIR.fullmatch(token)
        if not match:
            fail(f"{where}: cannot read '{token}' as name=value")
        add(f"{label} {match.group(1)}" if label else match.group(1), int(match.group(2)), where, values)


def add(name, value, where, values):
    if name in values:
        fail(f"{where}: {name} is named twice")
    values[name] = value


def page_values(path):
    lines = read(path).splitlines()
    marked = [number for number, line in enumerate(lines) if line.lstrip().startswith("<!--") and MARKER in line]
    if len(marked) != 1:
        fail(f"{path} holds {len(marked)} HTML comments naming {MARKER}, expected exactly one")

    number = marked[0] + 1
    while number < len(lines) and not lines[number].strip():
        number += 1
    if number == len(lines) or not lines[number].startswith("```"):
        fail(f"{path}:{marked[0] + 1}: no fenced code block follows the comment naming {MARKER}")

    values = {}
    number += 1
    while number < len(lines) and not lines[number].startswith("```"):
        words = lines[number].split(maxsplit=1)
        if words and words[0].endswith(":"):
            add_pairs(words[1] if len(words) > 1 else "", words[0][:-1], f"{path}:{number + 1}", values)
        elif words:
            add_pairs(lines[number], "", f"{path}:{number + 1}", values)
        number += 1
    if number == len(lines):
        fail(f"{path}: the block after the comment naming {MARKER} is never closed")
    return values


def case_values(path):
    text = read(path)
    opening = text.find(f'TEST_CASE("{CASE}"')
    if opening < 0:
        fail(f'{path} holds no case named "{CASE}"')
    closing = text.find("\n}\n", opening)
    if closing < 0:
        fail(f'{path}: the case "{CASE}" is never closed')
    body = LITERAL_OR_COMMENT.sub(lambda match: match.group(1) or "", text[opening:closing])

    values = {}
    for section, name, value in COUNTER.findall(body):
        add(f"{section[:-1]} {name}" if section.endswith(":") else name, int(value), path, values)
    for label, held in BLOCK.findall(body):
        if not label.endswith(":"):
            fail(f"{path}: the block '{label}' is not named by its label")
        add_pairs(held, label[:-1], path, values)
    return values


def self_test():
    """Hold the comparison to its verdicts, on pages and cases it writes for the purpose."""
    # A page and a case that agree, naming their values in each form the real
    # ones use: counters of the dump's unlabelled first line, a single counter
    # of a labelled block, and a whole block compared as a single text. Each
    # also contains characters beyond ASCII, meaning that saving the same text
    # in Windows-1252 makes a file that cannot be read as UTF-8.
    page = "\n".join([
        "# Measurement",
        "",
        "The run’s values – one line for each block of the dump.",
        "",
        f"<!-- tools/{MARKER} compares this block with the case. -->",
        "",
        "```",
        "deduce_calls=39869 facts_created=15406",
        "extract: ok=165298",
        "evaluate: calls=135611 leaf_conditions=123425",
        "```",
        "",
    ])
    case = "\n".join([
        "// The run’s values – one comparison for each counter.",
        f'TEST_CASE("{CASE}" * doctest::test_suite("slow"))',
        "{",
        '    CHECK(prof_counter(prof, "rules_applied", "deduce_calls") == 39869);',
        '    CHECK(prof_counter(prof, "rules_applied", "facts_created") == 15406);',
        '    CHECK(prof_counter(prof, "extract:", "ok") == 165298);',
        '    CHECK(prof_block(prof, "evaluate:") == "calls=135611 leaf_conditions=123425");',
        "}",
        "",
    ])
    agreement = "The measurement page and the case name the same 5 values of the invariant set."
    # The case's values are the comparisons its code makes. One found in a
    # comment, such as a comparison against an earlier value, does not
    # represent a value the case holds, nor does one in a later case of the
    # file: the case ends at its closing brace. If counted, either would stand
    # for a value the case does not hold.
    later = "\n".join([
        "",
        'TEST_CASE("performance guard: a later case")',
        "{",
        '    CHECK(prof_counter(prof, "genuine:", "walks") == 0);',
        "}",
        "",
    ])
    compared = [
        ("the two agree", page, case, 0, agreement),
        ("a comparison in a line comment of the case", page,
         case.replace("{\n", '{\n    // CHECK(prof_counter(prof, "rules_applied", "seminaive_seeds") == 352925);\n'), 0, agreement),
        ("a comparison in a block comment of the case", page,
         case.replace("{\n", '{\n    /* CHECK(prof_counter(prof, "extract:", "template_rejects") == 8705); */\n'), 0, agreement),
        ("a comparison in a later case of the file", page, case + later, 0, agreement),
        ("a value only the page names",
         page.replace("facts_created=15406", "facts_created=15406 seminaive_seeds=352931"), case, 1,
         "error: the page names seminaive_seeds = 352931, and the case does not hold it"),
        ("a value only the case holds",
         page, case.replace("leaf_conditions=123425", "leaf_conditions=123425 conjunction_sets=520"), 1,
         "error: the case holds evaluate conjunction_sets = 520, and the page does not name it"),
        ("a value that differs between the two", page, case.replace("== 15406", "== 15407"), 1,
         "error: the page names facts_created = 15406, and the case holds 15407"),
    ]

    failures = []
    with tempfile.TemporaryDirectory(prefix="zelph_invariant_values_") as directory:
        def write(name, text, encoding="utf-8"):
            path = os.path.join(directory, name)
            with open(path, "w", encoding=encoding) as handle:
                handle.write(text)
            return path

        def verdict(page_path, case_path):
            """The exit code of the check on two files and what it printed, as ctest sees them."""
            done = subprocess.run([sys.executable, os.path.abspath(__file__), page_path, case_path],
                                  capture_output=True)
            return done.returncode, (done.stdout + done.stderr).decode("utf-8", errors="replace")

        for number, (name, page_text, case_text, expected_code, expected_line) in enumerate(compared):
            code, said = verdict(write(f"page{number}.md", page_text), write(f"case{number}.cpp", case_text))
            lines = said.splitlines()
            # Each difference appears on a line by itself, so a difference
            # reported that is absent fails the case in the same way as one that
            # is omitted.
            differences = [line for line in lines if line.startswith(("error: the page names ", "error: the case holds "))]
            expected_differences = [expected_line] if expected_code == 1 else []
            if code != expected_code or expected_line not in lines or differences != expected_differences:
                failures.append(f"{name}: expected exit {expected_code} and {expected_line!r}, got exit {code}: {said!r}")

        # A file the check cannot read leaves the question open, regardless of
        # what the other one holds: a file moved while CMakeLists.txt still
        # refers to its old path, or one an editor saved using a code page. If
        # taken as agreement, either would switch the check off without comment.
        good_page = write("page.md", page)
        good_case = write("case.cpp", case)
        unreadable = [
            ("the page is missing", os.path.join(directory, "absent.md"), good_case, "absent.md"),
            ("the case's file is missing", good_page, os.path.join(directory, "absent.cpp"), "absent.cpp"),
            ("the page is saved in Windows-1252", write("cp1252.md", page, "cp1252"), good_case, "cp1252.md"),
            ("the case's file is saved in Windows-1252", good_page, write("cp1252.cpp", case, "cp1252"), "cp1252.cpp"),
        ]
        for name, page_path, case_path, unread in unreadable:
            code, said = verdict(page_path, case_path)
            reported = [line for line in said.splitlines() if line.startswith("error: cannot read ")]
            if code != 2 or len(reported) != 1 or unread not in reported[0] or "name the same" in said:
                failures.append(f"{name}: expected exit 2 and that it cannot read {unread}, got exit {code}: {said!r}")

        # A value named twice also leaves the question open: the comparison
        # reads one value for each name, so an old value left beside the new
        # one would go unread, and the page could continue naming it. The case
        # reaches a counter in two ways, either alone or within its block, and
        # either can name it a second time.
        twice = [
            ("the page names a value twice",
             write("twice.md", page.replace("extract: ok=165298", "extract: ok=167040\nextract: ok=165298")),
             good_case, "twice.md", "extract ok"),
            ("the case holds a counter twice", good_page,
             write("twice.cpp", case.replace("}\n", '    CHECK(prof_counter(prof, "extract:", "ok") == 165298);\n}\n')),
             "twice.cpp", "extract ok"),
            ("the case holds a counter alone and within its block", good_page,
             write("alone.cpp", case.replace("}\n", '    CHECK(prof_counter(prof, "evaluate:", "calls") == 135611);\n}\n')),
             "alone.cpp", "evaluate calls"),
        ]
        for name, page_path, case_path, named_in, value in twice:
            code, said = verdict(page_path, case_path)
            reported = [line for line in said.splitlines() if line.startswith("error: ") and line.endswith(f": {value} is named twice")]
            if code != 2 or len(reported) != 1 or named_in not in reported[0] or "name the same" in said:
                failures.append(f"{name}: expected exit 2 and that {named_in} names {value} twice, got exit {code}: {said!r}")

    if failures:
        for failure in failures:
            print(f"error: {failure}", file=sys.stderr)
        return 1
    print(f"{len(compared) + len(unreadable) + len(twice)} cases: the check reports each difference, each file it cannot "
          f"read and each value named twice, and takes the case's values from its code alone.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("page", nargs="?", help="the measurement page to read (mkdocs/docs/internals/measurement.md)")
    parser.add_argument("guard", nargs="?", help="the source holding the case (src/test/test_performance_guard.cpp)")
    parser.add_argument("--self-test", action="store_true", help="check the comparison itself on pages and cases it writes")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.page or not args.guard:
        fail("name the measurement page and the source holding the case")

    named = page_values(args.page)
    held = case_values(args.guard)
    if not named:
        fail(f"{args.page}: the block after the comment naming {MARKER} names no value")
    if not held:
        fail(f'{args.guard}: the case "{CASE}" holds no value')

    differences = []
    for name, value in named.items():
        if name not in held:
            differences.append(f"the page names {name} = {value}, and the case does not hold it")
        elif held[name] != value:
            differences.append(f"the page names {name} = {value}, and the case holds {held[name]}")
    for name, value in held.items():
        if name not in named:
            differences.append(f"the case holds {name} = {value}, and the page does not name it")

    if differences:
        for difference in differences:
            print(f"error: {difference}", file=sys.stderr)
        print(f'error: {args.page} and the case "{CASE}" in {args.guard} name different values; '
              f"a change that moves the set brings its new values into both", file=sys.stderr)
        return 1

    print(f"The measurement page and the case name the same {len(named)} values of the invariant set.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
