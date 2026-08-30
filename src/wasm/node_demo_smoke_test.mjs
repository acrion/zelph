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

// Press every demo button of the playground, in order, against the real
// wasm module -- the same entry points src/wasm/web/zelph-worker.js uses.
//
// The rail's own semantics are reproduced: `requiresReset` types `.new`
// first, everything else runs on whatever the previous buttons left behind,
// and a silent `.node` follows each batch to refresh the graph panel. That
// last call is not decoration: it renders the last output node, so a
// rendering fault reaches this driver exactly as it reaches the page.
//
// A demo button whose commands produce an engine error is a broken button,
// and nothing else in the repository presses them.
//
//   node src/wasm/node_demo_smoke_test.mjs build-wasm/bin/zelph.mjs
//
// Demos that need a network this environment does not have are skipped by
// name; see SKIP below.

import { pathToFileURL } from "node:url";
import path from "node:path";

const modulePath = process.argv[2];
if (!modulePath) {
  console.error("usage: node node_demo_smoke_test.mjs <path to zelph.mjs>");
  process.exit(2);
}

// Buttons that reach for data this repository does not ship.
const SKIP = /wikidata|\.load\s/i;

const demos = await import(
  pathToFileURL(path.resolve("src/wasm/web/demos.js")).href
);
const createZelphModule = (await import(pathToFileURL(path.resolve(modulePath)).href))
  .default;

let captured = "";
// The third argument says whether the engine ended the line; without it the
// whole session arrives as one string and a per-line scan finds nothing.
const emit = (_channel, text, newline) => {
  captured += text + (newline ? "\n" : "");
};

const Module = await createZelphModule({
  zelphOutput: emit,
  print: (text) => emit(0, text, true),
  printErr: (text) => emit(1, text, true),
});

const processLine = Module.cwrap("zelph_process", null, ["string"]);
const takeGraphPath = Module.cwrap("zelph_take_graph_html_path", "string", []);

let pressed = 0;
let skipped = 0;
const failures = [];

for (const group of demos.DEMO_GROUPS) {
  for (const button of group.buttons) {
    const commands = String(button.command).split("\n");
    if (commands.some((c) => SKIP.test(c))) {
      skipped++;
      continue;
    }

    captured = "";
    if (button.requiresReset) processLine(".new");
    for (const command of commands) processLine(command);
    const own = captured;

    // The worker refreshes the graph panel with a silent `.node` after every
    // batch and suppresses its output, so whatever that call says is not
    // something a visitor sees -- but it still has to be MADE, because it is
    // what renders the last output node and therefore what a rendering fault
    // would go through.
    processLine(".node");
    takeGraphPath();

    // The engine reports a refused statement on its own channel and keeps
    // going, so the exit code says nothing -- the text has to be read.
    const bad = own
      .split("\n")
      .filter((l) => /^Error in line |^error: |Unknown command/.test(l.trim()));
    if (bad.length > 0) failures.push([`${button.id} ${button.label}`, bad]);
    pressed++;
  }
}

console.log(`pressed ${pressed} demo button(s), skipped ${skipped}`);
for (const [name, lines] of failures) {
  console.log(`FAIL ${name}`);
  for (const l of lines) console.log(`     ${l}`);
}
console.log(failures.length === 0 ? "-- done --" : `-- ${failures.length} broken button(s) --`);
process.exit(failures.length === 0 ? 0 : 1);
