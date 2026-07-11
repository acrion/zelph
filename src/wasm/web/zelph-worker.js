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

// zelph engine worker: runs the WebAssembly module off the main thread so
// long reasoning runs never freeze the page. Output events stream to the
// main thread as messages; the main thread owns terminal, input and buttons.
//
// After every visible command batch the worker silently runs `.node`, which
// makes zelph regenerate the Mermaid HTML for the last output node (written
// to MEMFS under /tmp). The worker scans all output - visible and
// suppressed - for `file://...html` links, keeps only the LAST one (some
// commands print several), reads the file content and ships it to the main
// thread for display in the graph panel.

import createZelphModule from "./zelph.mjs";

let suppress = false; // true while running the internal .node
let lastMermaidPath = null; // last file://...html seen in the current batch

const MERMAID_RE = /file:\/\/([^\x07\x1b]*?\.html)/g; // stop at ESC/BEL of the OSC 8 link

function scanForMermaid(text) {
  for (const m of text.matchAll(MERMAID_RE)) lastMermaidPath = m[1];
}

function emit(channel, text, newline) {
  scanForMermaid(text);
  if (!suppress) postMessage({ type: "output", channel, text, newline });
}

const Module = await createZelphModule({
  zelphOutput: emit,
  // Raw stdio (e.g. Janet's (print ...)) bypasses the zelph output handler.
  print: (text) => emit(0, text, true),
  printErr: (text) => emit(1, text, true),
});

const processLine = Module.cwrap("zelph_process", null, ["string"]);
const promptStr = Module.cwrap("zelph_prompt", "string", []);
const isAccumulating = () =>
  Module.ccall("zelph_is_accumulating", "number", [], []) !== 0;

// Silently refresh the graph of the last output node. Never inject while a
// multi-line statement, Janet block or keyword block (e.g. sparql) is being
// accumulated: the line would be swallowed by the accumulation buffer
// instead of being executed as a command.
function autoNode() {
  if (isAccumulating()) return;
  suppress = true;
  try {
    processLine(".node");
  } catch (_) {
    /* silent by design */
  }
  suppress = false;
}

function publishMermaid() {
  if (!lastMermaidPath) return;
  try {
    const html = Module.FS.readFile(lastMermaidPath, { encoding: "utf8" });
    const label = decodeURIComponent(
      lastMermaidPath
        .split("/")
        .pop()
        .replace(/\.html$/, ""),
    );
    Module.FS.unlink(lastMermaidPath); // content delivered; keep MEMFS tidy
    if (html.includes("Syntax error in text")) {
      // Defensive: a generator hiccup (e.g. special characters in node
      // names) must never leave a broken graph on screen. No error message
      // by design; the main thread additionally checks the rendered result.
      postMessage({ type: "mermaid-clear" });
    } else {
      postMessage({ type: "mermaid", html, label });
    }
  } catch (_) {
    /* file may be missing if the command failed mid-way */
  }
}

self.onmessage = (e) => {
  const msg = e.data;

  if (msg.type === "process") {
    lastMermaidPath = null;
    const t0 = performance.now();
    try {
      for (const line of msg.lines) processLine(line);
    } catch (err) {
      postMessage({
        type: "output",
        channel: 1,
        text: String(err),
        newline: true,
      });
    }
    const ms = performance.now() - t0; // measure only the user's lines
    autoNode();
    publishMermaid();
    postMessage({
      type: "done",
      ms,
      prompt: promptStr(),
      accumulating: isAccumulating(),
    });
  } else if (msg.type === "reset") {
    Module.ccall("zelph_reset", null, [], []);
    postMessage({
      type: "done",
      ms: 0,
      prompt: promptStr(),
      accumulating: false,
    });
  }
};

// promptStr() lazily constructs the engine, so the heavy setup happens here
// in the worker, before we report ready.
postMessage({
  type: "ready",
  version: Module.ccall("zelph_version", "string", [], []),
  prompt: promptStr(),
});
