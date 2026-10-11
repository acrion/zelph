#!/usr/bin/env python3
"""Hold CITATION.cff to the version the build declares.

The version lives in two files that nothing ties together: ``project()`` in
CMakeLists.txt, which is the version the binary reports, and the ``version:``
key of CITATION.cff, which is the version a paper cites. A release that raises
one and forgets the other publishes a citation for a version other than the one
it ships, and nobody reading the citation can tell.

CMake passes its own ``PROJECT_VERSION``, so the comparison is made against the
value the build actually uses rather than against a second parse of
CMakeLists.txt.

Exit codes: 0 the two agree, 1 they differ, 2 the question could not be put.
"""

import argparse
import re
import sys


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def cited_version(path):
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        fail(f"cannot read {path}: {exc}")

    # Only a key positioned at the start of a line: "cff-version:" names the
    # version of the file format, while an indented "version:" would belong
    # to a nested element like a reference.
    found = re.findall(r'^version:\s*["\']?([^"\'\s#]+)["\']?\s*(?:#.*)?$', text, re.MULTILINE)
    if len(found) != 1:
        fail(f"{path} declares {len(found)} top-level 'version:' keys, expected exactly one")
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("citation", help="the CITATION.cff to read")
    parser.add_argument("project_version", help="the version CMake declares (PROJECT_VERSION)")
    args = parser.parse_args()

    cited = cited_version(args.citation)
    if cited != args.project_version:
        print(f"error: {args.citation} cites version {cited}, "
              f"but the build declares {args.project_version}", file=sys.stderr)
        return 1

    print(f"CITATION.cff and the build both declare version {cited}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
