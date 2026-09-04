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

// A fixture for the instruction set check, not a test of zelph. Built at the
// declared floor, it is what the check has to accept. Nothing here is called;
// what matters is the code the compiler emits for it.

#include <cstddef>

extern "C" double zelph_arch_probe_baseline(const double* a, const double* b, std::size_t n)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        sum += a[i] * b[i];
    }
    return sum;
}
