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

// The counterpart: built with AVX-512 intentionally, so the check has something
// it must refuse. This is the shape the released 1.0.0 library took, produced
// the way the toolchain produced it rather than by damaging a file.

#include <immintrin.h>

extern "C" void zelph_arch_probe_avx512(const double* a, const double* b, double* out)
{
    _mm512_storeu_pd(out, _mm512_add_pd(_mm512_loadu_pd(a), _mm512_loadu_pd(b)));
}
