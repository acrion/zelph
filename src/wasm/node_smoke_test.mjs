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

// The engine output goes to the console just as it has historically,
// with the exception that during the final .timer check, it is
// gathered instead.
let collected = null;
const Module = await createZelphModule({
  zelphOutput: (channel, text, newline) => {
    if (collected) collected.push(text);
    else (channel === 1 ? process.stderr : process.stdout).write(text + (newline ? "\n" : ""));
  },
});

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

// The `.timer` setting arrives at the playground via `zelph_report_timing`,
// invoked by the worker using the duration taken by a batch of lines. The
// engine decides whether a timing line appears and its format, just as it does
// for the terminal, so each configuration is passed here with a time span
// positioned on either side of the 10 ms boundary defined by `auto`.
const reportTiming = Module.cwrap("zelph_report_timing", null, ["number"]);
const timingLines = (setting, ms) => {
  processLine(`.timer ${setting}`);
  collected = [];
  reportTiming(ms);
  const lines = collected.filter((text) => /^-- .* --$/.test(text));
  collected = null;
  return lines;
};

let failures = 0;
for (const [setting, ms, expected] of [
  ["auto", 9.9, []],
  ["auto", 12.7, ["-- 12 ms --"]],
  ["on", 0.4, ["-- 0 ms --"]],
  ["off", 5000, []],
]) {
  const got = timingLines(setting, ms);
  if (JSON.stringify(got) !== JSON.stringify(expected)) {
    console.error(`FAIL .timer ${setting} after ${ms} ms: expected ${JSON.stringify(expected)}, got ${JSON.stringify(got)}`);
    failures++;
  }
}

console.log(failures === 0 ? "-- done --" : `-- ${failures} .timer case(s) failed --`);
process.exit(failures === 0 ? 0 : 1);
