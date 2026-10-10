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

// Tests the prerequisite dialog in the playground and the dependency
// bookkeeping underlying it, via the same functions the page invokes
// (src/wasm/web/rail.js). There is no requirement for a wasm module or a
// browser:
//
//   node src/wasm/node_prereq_dialog_test.mjs

import { test } from "node:test";
import assert from "node:assert/strict";

const { DEMO_GROUPS } = await import(new URL("./web/demos.js", import.meta.url));
const { missingPrereqs, press, prereqDialog } = await import(
  new URL("./web/rail.js", import.meta.url)
);

const byLabel = new Map(
  DEMO_GROUPS.flatMap((g) => g.buttons).map((b) => [b.label, b]),
);
const button = (label) => {
  const b = byLabel.get(label);
  assert.ok(b, `no demo button labelled ${label}`);
  return b;
};

// What the dialog would display upon pressing `label` after
// `pressedLabels` were pressed.
function dialogFor(label, pressedLabels = []) {
  const pressed = new Set();
  for (const l of pressedLabels) press(button(l), pressed);
  const missing = missingPrereqs(button(label), pressed);
  return missing.length === 0 ? null : prereqDialog(missing, DEMO_GROUPS);
}

test("a single missing button is asked for by name, its group in plain text", () => {
  const d = dialogFor("The map, at (0, 0, −1)");
  assert.equal(d.title, "Press another button first");
  assert.equal(
    d.listHTML,
    '<li><span class="prereq-step">Press button "Load mathematics"</span>' +
      ' (group "Symbolic Mathematics")</li>',
  );
});

test("two missing buttons turn the title into the plural, listed in rail order", () => {
  const d = dialogFor("SPARQL: primes");
  assert.equal(d.title, "Press other buttons first");
  assert.equal(
    d.listHTML,
    '<li><span class="prereq-step">Press button "Batch: test 2–20"</span>' +
      ' (group "Number Theory and Meta-Rules")</li>' +
      '<li><span class="prereq-step">Press button "Load SPARQL"</span>' +
      ' (group "SPARQL over Derived Facts")</li>',
  );
});

// The dialog names the next button back and nothing behind it: when
// pressed, that button requests its own prerequisites. Selecting "Run
// anyway" constitutes a press, meaning a user who opted for it -- perhaps
// after manually entering `.import math` -- will not be blocked again
// during the following step.
test("only direct prerequisites are named", () => {
  const d = dialogFor("…and at (1, −3, 26)");
  assert.match(d.listHTML, /Press button "The map, at \(0, 0, −1\)"/);
  assert.doesNotMatch(d.listHTML, /Load mathematics/);
});

test("a step after 'Run anyway' brings up no second dialog", () => {
  assert.equal(dialogFor("…and at (1, −3, 26)", ["The map, at (0, 0, −1)"]), null);
});

// The rail is ordered so that the dialog is encountered solely by a
// visitor who skips ahead. A button whose `requires` refers to a button
// located below it, or to an identifier that does not exist, would make the
// plain top-to-bottom walk fail.
test("pressing every button from top to bottom never brings up the dialog", () => {
  const pressed = new Set();
  for (const group of DEMO_GROUPS) {
    for (const b of group.buttons) {
      assert.deepEqual(missingPrereqs(b, pressed), [], `${b.id} ${b.label}`);
      press(b, pressed);
    }
  }
});
