#!/usr/bin/env python3
"""Refuse a binary that needs more of the CPU than the build declares.

zelph pins an instruction set floor for released x86-64 builds
(ZELPH_X86_BASELINE, see src/lib/CMakeLists.txt). This script holds a built
artefact against that floor and exits non-zero when the artefact needs more,
whether the extra instructions come from a compiler flag, a dependency or a
toolchain default.

The floor is read off the encoding rather than off mnemonic names. In 64-bit
mode the opcode 0x62 is the EVEX prefix and nothing else, and 0xC4 and 0xC5 are
the VEX prefixes and nothing else, because the legacy instructions that once
used those opcodes do not exist there. So:

    x86-64-v3   rejects EVEX, that is AVX-512 and what came after it
    x86-64-v2   rejects EVEX and VEX, that is AVX, AVX2, FMA and BMI as well

Opmask and 512-bit registers are matched in the operands as well, because the
opmask moves (kmovb and its relatives) are VEX encoded and would otherwise pass
a v3 floor.

What this does not see: extensions that keep the legacy encoding and are not in
the list below. The list holds those a compiler reaches for on its own; an
intrinsic for something more exotic can still slip through. At the v2 floor the
same applies to lzcnt and tzcnt, which are v3 but legacy encoded.

Exit codes: 0 the artefact fits, 1 it does not, 2 the question could not be put.
"""

import argparse
import re
import shutil
import subprocess
import sys

VEX_PREFIXES = {0xC4, 0xC5}
EVEX_PREFIX = 0x62

FLOORS = {
    "x86-64-v2": VEX_PREFIXES | {EVEX_PREFIX},
    "x86-64-v3": {EVEX_PREFIX},
}

# Extensions that a compiler autonomously invokes and that keep the legacy
# encoding, so neither the VEX nor the EVEX rule above sees them.
LEGACY_ABOVE_V3 = {
    "adcx", "adox",
    "aesenc", "aesenclast", "aesdec", "aesdeclast", "aesimc", "aeskeygenassist",
    "pclmulqdq",
    "sha1rnds4", "sha1nexte", "sha1msg1", "sha1msg2",
    "sha256rnds2", "sha256msg1", "sha256msg2",
    "rdrand", "rdseed", "rdpid",
    "clwb", "cldemote", "movdiri", "movdir64b", "serialize",
    "prefetchwt1", "ptwrite", "umonitor", "umwait", "tpause",
}

WIDE_REGISTER = re.compile(r"%zmm[0-9]+")
OPMASK_REGISTER = re.compile(r"%k[0-7]\b")


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def find_objdump(explicit):
    if explicit:
        return explicit
    for name in ("objdump", "llvm-objdump", "gobjdump"):
        found = shutil.which(name)
        if found:
            return found
    return None


def run(objdump, *args):
    try:
        done = subprocess.run([objdump, *args], capture_output=True, check=False,
                              encoding="utf-8", errors="replace")
    except OSError as exc:
        fail(f"cannot run {objdump}: {exc}")
    if done.returncode != 0:
        fail(f"{objdump} {' '.join(args)} failed: {done.stderr.strip()}")
    return done.stdout


def violations(disassembly, rejected_prefixes, floor):
    found = []
    for line in disassembly.splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[0].strip().endswith(":"):
            continue

        # A long instruction wraps, and the continuation lines carry bytes
        # without a mnemonic. The prefix that decides belongs to the first line.
        text = parts[2].strip()
        if not text:
            continue

        raw = parts[1].split()
        if not raw:
            continue
        try:
            first = int(raw[0], 16)
        except ValueError:
            continue

        mnemonic, _, operands = text.partition(" ")
        operands = operands.split("#")[0]

        reason = None
        if first in rejected_prefixes:
            reason = "EVEX encoded" if first == EVEX_PREFIX else "VEX encoded"
        elif WIDE_REGISTER.search(operands):
            reason = "512 bit register"
        elif OPMASK_REGISTER.search(operands):
            reason = "opmask register"
        elif floor == "x86-64-v3" and mnemonic in LEGACY_ABOVE_V3:
            reason = f"{mnemonic} is above the floor"

        if reason:
            found.append((parts[0].strip().rstrip(":"), text, reason))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary")
    parser.add_argument("--floor", default="x86-64-v3", choices=sorted(FLOORS))
    parser.add_argument("--objdump", default=None)
    parser.add_argument("--max-report", type=int, default=10)
    args = parser.parse_args()

    objdump = find_objdump(args.objdump)
    if not objdump:
        fail("no objdump found; pass --objdump")

    header = run(objdump, "-f", args.binary)
    if "i386:x86-64" not in header and "x86-64" not in header:
        fail(f"{args.binary} is not an x86-64 object, so the {args.floor} floor says nothing about it")

    found = violations(run(objdump, "-d", args.binary), FLOORS[args.floor], args.floor)
    if not found:
        print(f"{args.binary}: fits within {args.floor}")
        return 0

    print(f"{args.binary}: {len(found)} instruction(s) above {args.floor}", file=sys.stderr)
    for address, text, reason in found[: args.max_report]:
        print(f"  {address}: {text}   [{reason}]", file=sys.stderr)
    if len(found) > args.max_report:
        print(f"  ... and {len(found) - args.max_report} more", file=sys.stderr)
    return 1


if __name__ == "__main__":
    # A crash here must not resemble a verdict. Without this, an exception leaves
    # with 1, which is the code indicating the artefact failed the check, and the
    # build then reports a violation that was never established.
    try:
        exit_code = main()
    except SystemExit:
        raise
    except Exception as exc:
        fail(f"the check itself broke: {exc.__class__.__name__}: {exc}")
    sys.exit(exit_code)
