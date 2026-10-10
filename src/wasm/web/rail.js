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

// The demo rail's bookkeeping independent of the DOM: which buttons are
// considered pressed, which prerequisites are missing, and what the
// prerequisite dialog reports regarding them. playground.js drives the page
// with it, while node_prereq_dialog_test.mjs operates it outside a browser
// environment.

export function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// Only the button's own `requires` matter, not those of the ones it
// depends on: the dialog names the next button back, and that one
// requests its own prerequisites upon being pressed. A visitor who chose
// "Run anyway" -- perhaps after typing the prerequisites manually -- is
// not stopped again one step further on.
export function missingPrereqs(button, pressed) {
  return (button.requires || []).filter((id) => !pressed.has(id));
}

// A demonstration operating within an empty network reverses all prior
// ones, so they are forgotten too; this ensures dependency markers remain
// truthful.
export function press(button, pressed) {
  if (button.requiresReset) pressed.clear();
  pressed.add(button.id);
}

// The dialog requests each missing button by name, following the rail
// order, and names its group since the group might still be collapsed.
export function prereqDialog(missing, groups) {
  const items = [];
  for (const group of groups) {
    for (const b of group.buttons) {
      if (!missing.includes(b.id)) continue;
      items.push(
        `<li><span class="prereq-step">Press button "${escapeHtml(b.label)}"</span>` +
          ` (group "${escapeHtml(group.title)}")</li>`,
      );
    }
  }
  return {
    title:
      items.length > 1 ? "Press other buttons first" : "Press another button first",
    listHTML: items.join(""),
  };
}
