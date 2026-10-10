#!/usr/bin/env python3
"""Hold the zelph binary to the allocator it names.

``zelph -v`` lists the third-party software the binary incorporates, mimalloc
among it. That line used to be compiled from whichever ``<mimalloc.h>`` the
compiler found first -- the system header, because only the app is linked
against the pinned copy -- and decoded with the wrong divisors on top, so that
3.4.5 printed as 30.4.5. A paper's artifact recorded exactly that line as the
allocator of the runs it documents. Neither half could be seen from the test
suite, which links no mimalloc at all; only the app can be asked.

    check_version_banner.py <zelph> --mimalloc 3.4.5

Exit codes: 0 the banner names that version, 1 it names another one or none,
2 the question could not be put.
"""

import argparse
import os
import re
import subprocess
import sys


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="the zelph executable to question")
    parser.add_argument("--mimalloc", required=True,
                        help="the version the build pins and links, e.g. 3.4.5")
    args = parser.parse_args()

    if not os.path.isfile(args.binary):
        fail(f"no such binary: {args.binary}")

    # rlwrap would re-exec the process and take the output with it.
    os.environ["ZELPH_NO_RLWRAP"] = "1"

    try:
        done = subprocess.run([args.binary, "-v"], capture_output=True, check=False,
                              encoding="utf-8", errors="replace")
    except OSError as exc:
        fail(f"cannot run {args.binary}: {exc}")

    output = (done.stdout or "") + (done.stderr or "")
    if done.returncode != 0:
        print(f"error: {args.binary} -v exited {done.returncode}:\n{output}", file=sys.stderr)
        return 1

    named = re.findall(r"^mimalloc \(v([^)]*)\)", output, re.MULTILINE)
    if named != [args.mimalloc]:
        print(f"error: the banner names mimalloc {named or 'nowhere'}, "
              f"the binary is linked against {args.mimalloc}", file=sys.stderr)
        return 1

    print(f"the banner names mimalloc {args.mimalloc}, the version the binary is linked against")
    return 0


if __name__ == "__main__":
    sys.exit(main())
