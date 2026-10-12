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

The disassembly is a linear sweep, so it also decodes data that a compiler put
into the code section. MSVC puts the jump tables of a switch there, as four-byte
offsets; an entry whose low byte is 0x62 then reads like an EVEX prefix, and
objdump calls the sequence (bad) because it is no instruction. A sequence the
disassembler cannot decode is not counted, but only after the disassembler has
shown that it decodes EVEX: otherwise (bad) may be an instruction it does not
know, and it counts as before. A table entry that happens to decode as a valid
instruction is still counted.

Exit codes: 0 the artefact fits, 1 it does not, 2 the question could not be put.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

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

# vaddps %zmm2,%zmm1,%zmm0: AVX-512F, the instruction set extension that a
# compiler reaches for first above x86-64-v3.
EVEX_SAMPLE = bytes.fromhex("62f1744858c2")

# What is displayed by GNU objdump and llvm-objdump when they encounter
# bytes that they are unable to decode.
UNDECODABLE = {"(bad)", ".byte", "<unknown>"}

INSTRUCTION_LINE = re.compile(r"^\s*([0-9a-f]+):\s*([0-9a-f]{2}(?: [0-9a-f]{2})*)\s*(?:\t(.*))?$")

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


def decodes_evex(objdump):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as sample:
        sample.write(EVEX_SAMPLE)
    try:
        done = subprocess.run([objdump, "-D", "-b", "binary", "-m", "i386:x86-64", sample.name],
                              capture_output=True, check=False, encoding="utf-8", errors="replace")
    except OSError:
        return False
    finally:
        os.unlink(sample.name)
    return (done.returncode == 0 and "%zmm" in done.stdout
            and not any(word in done.stdout for word in UNDECODABLE))


def violations(disassembly, rejected_prefixes, floor, undecodable_is_data):
    found = []
    undecodable = 0
    for line in disassembly.splitlines():
        # GNU objdump uses tabs to distinguish address, bytes, and instruction;
        # llvm-objdump puts a space following the address and employs a tab
        # between the mnemonic and its operands. Simply splitting on tabs
        # failed to match any line from the latter, resulting in a library
        # containing AVX-512 instructions going undetected.
        listed = INSTRUCTION_LINE.match(line)
        if not listed:
            continue
        address, raw, text = listed.groups()

        # A long instruction wraps, and the continuation lines carry bytes
        # without a mnemonic. The prefix that decides belongs to the first line.
        text = " ".join((text or "").split())
        if not text:
            continue
        first = int(raw[:2], 16)

        mnemonic, _, operands = text.partition(" ")
        operands = operands.split("#")[0]

        if mnemonic in UNDECODABLE and undecodable_is_data:
            if first in rejected_prefixes:
                undecodable += 1
            continue

        reason = None
        if first in rejected_prefixes:
            reason = "EVEX encoded" if first == EVEX_PREFIX else "VEX encoded"
        elif WIDE_REGISTER.search(operands):
            reason = "512 bit register"
        elif OPMASK_REGISTER.search(operands):
            reason = "opmask register"
        elif floor == "x86-64-v3" and (mnemonic in LEGACY_ABOVE_V3
                                       or (mnemonic[-1:] in "bwlq" and mnemonic[:-1] in LEGACY_ABOVE_V3)):
            reason = f"{mnemonic} is above the floor"

        if reason:
            found.append((address, text, reason))
    return found, undecodable


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

    found, undecodable = violations(run(objdump, "-d", args.binary), FLOORS[args.floor], args.floor,
                                    decodes_evex(objdump))
    if undecodable:
        print(f"{args.binary}: {undecodable} undecodable byte sequence(s) with a VEX or EVEX prefix, "
              f"taken for data in the code section")
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
