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

// zelph playground - main-thread UI. The engine runs in a Web Worker
// (zelph-worker.js). This file owns the terminal (output only), the input
// line with history, the demo button rail, the info panel (hover preview
// vs. last executed command) and the graph panel fed by the worker's
// Mermaid messages.

import { DEMO_GROUPS } from "./demos.js";

// --- terminal (output only) --------------------------------------------------

const term = new Terminal({
  convertEol: true,
  disableStdin: true,
  fontSize: 15,
  scrollback: 8000,
  theme: { background: "#1e1e1e" },
});
const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
term.open(document.getElementById("terminal"));
fit.fit();
term.write("\x1b[?25l"); // hide the terminal cursor: input lives below

// Refit on ANY size change of the terminal container - not just window
// resizes. The info panel above the input bar changes height with its
// content; without this, the xterm canvas keeps its old pixel size.
new ResizeObserver(() => {
  try {
    fit.fit();
  } catch (_) {}
}).observe(document.getElementById("terminal"));

const DIM = "\x1b[2m",
  RED = "\x1b[31m",
  RESET = "\x1b[0m";
const CH_ERR = 1,
  CH_DIAG = 2,
  CH_PROMPT = 3;

function writeEvent(channel, text, newline) {
  if (channel === CH_PROMPT) return; // the prompt is rendered by the input bar
  let styled = text;
  if (channel === CH_ERR) styled = RED + text + RESET;
  if (channel === CH_DIAG) styled = DIM + text + RESET;
  term.write(styled + (newline ? "\n" : ""));
}

// Mirrors format_duration in src/app/main.cpp (10 ms threshold).
function formatDuration(ms) {
  if (ms < 1000) return `${Math.round(ms)} ms`;
  const s = ms / 1000;
  if (s < 60) return `${s.toFixed(3)} s`;
  const m = Math.floor(s / 60);
  return `${m}m${(s - m * 60).toFixed(3)}s`;
}

// --- elements & state ----------------------------------------------------------

const promptEl = document.getElementById("prompt");
const inputEl = document.getElementById("input");
const formEl = document.getElementById("inputbar");
const infoEl = document.getElementById("info");
const railEl = document.getElementById("rail");
const stopBtn = document.getElementById("stop");
const resetBtn = document.getElementById("reset");
const graphFrame = document.getElementById("graphframe");
const graphLabel = document.getElementById("graphlabel");
const dlg = document.getElementById("prereq-dialog");
const dlgList = document.getElementById("prereq-list");
const dlgRun = document.getElementById("prereq-run");
const dlgCancel = document.getElementById("prereq-cancel");

let worker = null;
let ready = false;
let busy = true; // until the worker reports ready
let prompt = "zelph> ";
let accumulating = false;
const clicked = new Set(); // demo buttons pressed since the last reset
const history = [];
let histPos = 0;
let pendingDemo = null; // demo awaiting prerequisite confirmation

// Whether to re-focus the input after a command is decided behavior-based,
// not capability-based: media queries like (hover)/(pointer) are
// self-reported device claims and are wrong on some mobile browsers
// (notably Firefox on Android). The pointerType of the user's most recent
// interaction is ground truth: after a touch tap we must never call
// focus() (it pops up the soft keyboard); after a mouse click - or at page
// load, before any interaction - we may.
let lastPointerType = "mouse";
addEventListener(
  "pointerdown",
  (e) => {
    lastPointerType = e.pointerType || "mouse";
  },
  { capture: true, passive: true },
);

function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// --- info panel: hover preview vs. last executed command -----------------------

const INFO_IDLE =
  "Hover over a demo button to preview its command and explanation; click to run it.";
let persistedInfo = INFO_IDLE;

function infoHTML(head, command, doc) {
  return (
    `<div class="info-head">${head}</div>` +
    `<pre>${escapeHtml(command)}</pre>` +
    (doc ? doc : "")
  );
}
function demoDoc(b) {
  return `<strong>${b.label}</strong> &mdash; ${b.info}`;
}
function setInfoHTML(html) {
  infoEl.innerHTML = html;
}
setInfoHTML(persistedInfo);

// --- graph panel -----------------------------------------------------------------

const GRAPH_PLACEHOLDER = `<!DOCTYPE html><html><body style="background:#1e1e1e;color:#666;
font:13px system-ui,sans-serif;display:flex;align-items:center;justify-content:center;
height:100%;margin:0;text-align:center">The graph of the last result node appears here
after each command (drag to pan, scroll to zoom).</body></html>`;

let graphCheckTimer = null;

function clearGraph() {
  clearInterval(graphCheckTimer);
  graphFrame.srcdoc = GRAPH_PLACEHOLDER;
  graphLabel.textContent = "";
}

function showGraph(html, label) {
  graphFrame.srcdoc = html;
  graphLabel.textContent = label || "";
  // Mermaid renders client-side; a malformed diagram only reveals itself in
  // the rendered document. Poll briefly and silently fall back to the
  // placeholder on failure (no error message by design).
  clearInterval(graphCheckTimer);
  let tries = 0;
  graphCheckTimer = setInterval(() => {
    let text = "";
    try {
      text = graphFrame.contentDocument?.body?.textContent || "";
    } catch (_) {}
    if (text.includes("Syntax error in text")) {
      clearGraph();
    } else if (++tries > 25) {
      clearInterval(graphCheckTimer);
    }
  }, 200);
}
clearGraph();

// --- busy handling -----------------------------------------------------------------

function refreshPrompt() {
  promptEl.textContent = busy ? "…" : accumulating ? "⋯" : prompt;
}

function setBusy(b) {
  busy = b;
  document.body.classList.toggle("busy", b);
  // Note: stopBtn is deliberately never disabled - it is the escape hatch
  // for long computations AND for being stuck in multi-line accumulation.
  updateButtons();
  refreshPrompt();
  if (!b && lastPointerType === "mouse") inputEl.focus({ preventScroll: true });
}

// --- worker lifecycle -----------------------------------------------------------------

function spawnWorker() {
  worker = new Worker("./zelph-worker.js", { type: "module" });
  worker.onmessage = (e) => {
    const m = e.data;
    if (m.type === "output") {
      writeEvent(m.channel, m.text, m.newline);
    } else if (m.type === "ready") {
      ready = true;
      prompt = m.prompt;
      accumulating = false;
      term.writeln(`zelph ${m.version} (WebAssembly)`);
      term.writeln(DIM + "-- engine ready --" + RESET);
      term.writeln("");
      setBusy(false);
    } else if (m.type === "done") {
      if (m.ms >= 10)
        term.writeln(DIM + `-- ${formatDuration(m.ms)} --` + RESET);
      prompt = m.prompt || "zelph> ";
      accumulating = m.accumulating;
      setBusy(false);
    } else if (m.type === "mermaid") {
      showGraph(m.html, m.label);
    } else if (m.type === "mermaid-clear") {
      clearGraph();
    }
  };
  worker.onerror = (e) => {
    term.writeln(RED + `worker error: ${e.message || e}` + RESET);
    setBusy(false);
  };
}

function runLines(lines, { echo = true, doc = null } = {}) {
  if (!ready || busy) return;

  // Every executed command - typed or button-triggered - joins the history
  // and becomes the "Last command" shown in the info panel.
  const raw = lines.join("\n");
  if (raw.trim() !== "") {
    history.push(raw);
    persistedInfo = infoHTML("Last command:", raw, doc);
    setInfoHTML(persistedInfo);
  }
  histPos = history.length;
  if (lines.some((l) => l.trim() === ".new")) clearGraph();

  if (echo) {
    lines.forEach((l, i) =>
      term.writeln((i === 0 && !accumulating ? prompt : "") + l),
    );
  }
  // xterm only follows new output while the viewport is at the bottom.
  // If the user has scrolled up, a command would otherwise run invisibly;
  // jumping down re-engages live scrolling (our implicit progress bar).
  term.scrollToBottom();
  setBusy(true);
  worker.postMessage({ type: "process", lines });
}

stopBtn.addEventListener("click", () => {
  if (!worker) return;
  worker.terminate();
  term.writeln("");
  term.writeln(
    RED + "-- interrupted: engine restarted, network cleared --" + RESET,
  );
  clicked.clear();
  clearGraph();
  // Unlike Reset, no command runs here, so there is no meaningful "last
  // command" anymore - fall back to the idle hint.
  persistedInfo = INFO_IDLE;
  setInfoHTML(persistedInfo);
  ready = false;
  accumulating = false;
  prompt = "zelph> ";
  setBusy(true); // released by the fresh worker's ready message
  spawnWorker();
  updateButtons();
});

resetBtn.addEventListener("click", () => {
  if (!ready || busy) return;
  clicked.clear();
  runLines([".new"], {
    doc:
      "<strong>Reset</strong> &mdash; Clears the network and starts " +
      "fresh. All facts, rules, and derivations are discarded.",
  });
  updateButtons();
});

// --- input bar with history ---------------------------------------------------------

function autosize() {
  inputEl.style.height = "auto";
  inputEl.style.height = Math.min(inputEl.scrollHeight, 120) + "px";
}
inputEl.addEventListener("input", autosize);

function submit() {
  if (!ready || busy) return; // ignore submits while running; keep the typed text
  const raw = inputEl.value;
  inputEl.value = "";
  autosize();

  const lines = raw.split("\n"); // pasted multi-line input runs line by line
  if (raw.trim() === "" && lines.length === 1 && !accumulating) {
    // Mirror the native REPL for a lone empty line. While accumulating,
    // empty lines are meaningful (they end keyword blocks) and pass through.
    term.writeln(prompt);
    term.writeln("type .help for help --");
    return;
  }
  runLines(lines);
}

formEl.addEventListener("submit", (e) => {
  e.preventDefault();
  submit();
});

inputEl.addEventListener("keydown", (e) => {
  // History navigation also works on unmodified multi-line entries (loaded
  // from history themselves); free-typed multi-line text is not hijacked.
  const untouched =
    inputEl.value === "" ||
    (histPos < history.length && inputEl.value === history[histPos]);
  const navigable = !inputEl.value.includes("\n") || untouched;

  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    submit();
  } else if (e.key === "ArrowUp" && navigable) {
    if (histPos > 0) {
      histPos--;
      inputEl.value = history[histPos];
      autosize();
      e.preventDefault();
    }
  } else if (e.key === "ArrowDown" && navigable) {
    if (histPos < history.length) {
      histPos++;
      inputEl.value = histPos === history.length ? "" : history[histPos];
      autosize();
      e.preventDefault();
    }
  }
});

// --- demo button rail ------------------------------------------------------------------

const buttonsById = new Map();
const groupOfId = new Map(); // demo id -> group title

function missingPrereqs(b) {
  return (b.requires || []).filter((id) => !clicked.has(id));
}

function labelOf(id) {
  return buttonsById.get(id)?._demo.label ?? id;
}

function updateButtons() {
  for (const [id, el] of buttonsById) {
    const b = el._demo;
    const miss = missingPrereqs(b);
    el.classList.toggle("done", clicked.has(id));
    el.classList.toggle("prereq", miss.length > 0); // CSS: prereq dot wins over done
    if (miss.length > 0) {
      el.title =
        "Depends on demo steps not pressed yet: " +
        miss.map((id2) => `${labelOf(id2)} (${groupOfId.get(id2)})`).join(", ");
    } else {
      el.removeAttribute("title"); // no tooltip when nothing is missing
    }
    el.disabled = busy || !ready;
  }
  resetBtn.disabled = busy || !ready;
}

function execDemo(b) {
  clicked.add(b.id);
  runLines(b.command.split("\n"), { doc: demoDoc(b) });
  updateButtons();
}

function runDemo(b) {
  if (busy || !ready) return;
  const miss = missingPrereqs(b);
  if (miss.length > 0) {
    pendingDemo = b;
    // Group the missing steps under their group headings so visitors know
    // where to find them (a group may still be collapsed).
    dlgList.innerHTML = DEMO_GROUPS.map((g) => {
      const inGroup = miss.filter((id) => groupOfId.get(id) === g.title);
      if (inGroup.length === 0) return "";
      return (
        `<li><strong>${escapeHtml(g.title)}</strong><ul>` +
        inGroup.map((id) => `<li>${escapeHtml(labelOf(id))}</li>`).join("") +
        "</ul></li>"
      );
    }).join("");
    dlg.showModal();
    return;
  }
  execDemo(b);
}

dlgRun.addEventListener("click", () => {
  dlg.close();
  const b = pendingDemo;
  pendingDemo = null;
  if (b) execDemo(b);
});
dlgCancel.addEventListener("click", () => {
  dlg.close();
  pendingDemo = null;
});

DEMO_GROUPS.forEach((group, gi) => {
  const details = document.createElement("details");
  if (gi === 0) details.open = true; // first group open on all devices
  const summary = document.createElement("summary");
  summary.textContent = group.title;
  details.appendChild(summary);
  for (const b of group.buttons) {
    const el = document.createElement("button");
    el.type = "button";
    el.textContent = b.label;
    el._demo = b;
    el.addEventListener("click", () => runDemo(b));
    el.addEventListener("mouseenter", () =>
      setInfoHTML(
        infoHTML(
          "Button under cursor &mdash; clicking will run:",
          b.command,
          demoDoc(b),
        ),
      ),
    );
    el.addEventListener("mouseleave", () => setInfoHTML(persistedInfo));
    buttonsById.set(b.id, el);
    groupOfId.set(b.id, group.title);
    details.appendChild(el);
  }
  railEl.appendChild(details);
});

// --- go -----------------------------------------------------------------------------------

term.writeln("loading zelph (WebAssembly)...");
setBusy(true);
spawnWorker();
updateButtons();
