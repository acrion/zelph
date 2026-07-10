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

// Usage (from the repository root, after building):
//   node src/wasm/node_smoke_test.mjs [path/to/zelph.mjs]

import { pathToFileURL } from "node:url";

const modulePath = process.argv[2] ?? "build-wasm/bin/zelph.mjs";
const { default: createZelphModule } = await import(
  pathToFileURL(modulePath).href
);

const Module = await createZelphModule();

const processLine = Module.cwrap("zelph_process", null, ["string"]);
const version = Module.ccall("zelph_version", "string", [], []);

console.log(`zelph ${version} (wasm)`);

const lines = [
  "(A trigger A) => (A step1 A)",
  "(A step1 A) => (A step2 A)",
  "(A trigger A, ¬(A step2 A)) => (A racewin A)",
  "x trigger x",
];

for (const line of lines) {
  console.log(`zelph> ${line}`);
  processLine(line);
}

// Auto-run has already reasoned after each line; one explicit run at the end
// is a cheap no-op at fixpoint and doubles as a smoke test for zelph_run.
Module.ccall("zelph_run", null, [], []);

console.log("-- done --");
