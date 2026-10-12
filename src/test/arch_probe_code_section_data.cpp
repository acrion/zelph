/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

// A fixture for the instruction set check: data in the code section, built
// at the declared floor. MSVC places the jump table of a switch statement
// following the function, using four-byte offsets relative to the image base,
// and a linear sweep interprets it as executable code. An entry with a low
// byte of 0x62 appears as an EVEX prefix, and objdump reports the sequence as
// (bad). The entries beneath exhibit this format, written out explicitly since
// no compiler on this platform generates such output.

asm(".text\n"
    ".globl zelph_arch_probe_code_section_data\n"
    "zelph_arch_probe_code_section_data:\n"
    ".long 0x00696562, 0x00696562, 0x00696570, 0x00696562\n"
    "ret\n");
