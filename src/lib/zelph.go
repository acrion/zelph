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

package zelph

// #cgo darwin CXXFLAGS: -std=gnu++1z -fPIC -I/Users/stefan/git/boost
// #cgo linux CXXFLAGS: -std=gnu++1z -fPIC -I/usr/include
// #cgo !linux,!darwin CXXFLAGS: -std=gnu++1z -fPIC -IC:/local/boost_1_70_0
// #cgo darwin LDFLAGS: -lstdc++fs -L/Users/stefan/git/boost/stage/lib -lboost_serialization
// void zelph_run();
// void zelph_process_c(const char* line, size_t len);
// static void zelph_process(_GoString_ line)
// {
//   zelph_process_c(line.p, line.n);
// }
import "C"

// Parse command and add it to the network
func Process(cmd string) {
	C.zelph_process(cmd)
}

// Apply rules and add any deductions to the network
func Run() {
	C.zelph_run()
}
