#!/usr/bin/env python3
"""Turn a zelph derivation export into a report.

zelph writes what a run derived with ``.run-export <file>``: JSON Lines, one
object per derived fact or contradiction, with no knowledge of what anyone
wants to do with them::

    {"kind":"deduction","conclusion":[SEG,...],"premises":[[SEG,...],...]}

A SEG is either a JSON string -- literal text of the rendering, brackets and
spacing -- or one of

    {"names":{"wikidata":"Q5","en":"human"}}   a node, in every language it
                                               is known by
    {"core":"!"}                               a node of zelph's own
                                               vocabulary (!, ~, =>, ...)

Everything a target format needs to decide is decided here, not in the
engine: which of a node's names to show, which of them is a URL, that
Wikidata properties are italicised, which file a line belongs in.

    md    (default) the MkDocs tree that the removed .run-md command used to
          write: one page per Wikidata identifier occurring in a conclusion,
          with the derivations that mention it and links between the pages.
    text  one flat line per derivation, premises first --
          "Q2 P279 Q3, Q1 P279 Q2 => Q1 P279 Q3". The shape the removed
          .run-file command wrote, and the starting point for a
          tokenizer-friendly training corpus: every identifier arrives as a
          discrete token, so substituting a compact encoding for it is a
          dictionary lookup rather than a parse.

Usage:
    zelph-derivations.py derivations.jsonl --format md   --out mkdocs/docs/report
    zelph-derivations.py derivations.jsonl --format text --out derivations.txt
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# The language zelph stores Wikidata identifiers under. Everything Wikidata-
# specific in this script hangs off this one name.
WIKIDATA_LANG = "wikidata"

HEADINGS = {"deduction": "Deductions", "contradiction": "Contradictions"}


def wikidata_url(identifier: str) -> str:
    kind = "Property:" if identifier.startswith("P") else ""
    return f"https://www.wikidata.org/wiki/{kind}{identifier}"


def node_id(segment: dict) -> str | None:
    """The Wikidata identifier of a node segment, if it has one."""
    return segment.get("names", {}).get(WIKIDATA_LANG)


def node_label(segment: dict) -> str:
    """What a human should read for this node: a natural-language name if the
    graph has one, otherwise whatever name it does have."""
    names = segment.get("names", {})
    for lang, name in names.items():
        if lang not in (WIKIDATA_LANG, "zelph"):
            return name
    return names.get(WIKIDATA_LANG) or names.get("zelph") or "?"


def render_plain(segments: list) -> str:
    out = []
    for seg in segments:
        if isinstance(seg, str):
            out.append(seg)
        elif "core" in seg:
            out.append(seg["core"])
        else:
            out.append(node_id(seg) or node_label(seg))
    return "".join(out)


def render_md(segments: list) -> str:
    out = []
    for seg in segments:
        if isinstance(seg, str):
            # A literal piece is rendering structure, not prose: escape what
            # Markdown would otherwise eat.
            out.append(re.sub(r"([*_`\[\]])", r"\\\1", seg))
        elif "core" in seg:
            out.append(f"`{seg['core']}`")
        else:
            identifier = node_id(seg)
            if identifier is None:
                out.append(node_label(seg))
                continue
            text = node_label(seg)
            if identifier.startswith("P"):
                text = f"*{text}*"
            out.append(f"[{text}]({identifier}.md)")
    return "".join(out)


def premises_text(premises: list, render) -> str:
    """Premises as the console prints them: a set in braces, unless the rule
    had a single condition and there is nothing to group."""
    parts = [render(p) for p in premises]
    if len(parts) == 1:
        return parts[0]
    return "{" + " ".join(parts) + "}"


def convert_md(records: list, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)

    # id -> heading -> ordered unique entries; plus the label to title the page
    pages: dict[str, dict[str, list[str]]] = {}
    labels: dict[str, str] = {}

    for rec in records:
        conclusion = rec.get("conclusion", [])
        heading = HEADINGS.get(rec.get("kind", ""), "Derivations")

        line = "- " + render_md(conclusion) + " ⇐ " + premises_text(rec.get("premises", []), render_md)

        # A page per identifier mentioned in the CONCLUSION -- the fact the
        # derivation is about. Premises are linked but do not pull a page in;
        # that is what made the reports readable rather than exhaustive.
        # A contradiction has no conclusion to speak of, so there it is the
        # premises that say which entities the report is about.
        anchors = conclusion
        if rec.get("kind") == "contradiction":
            anchors = [seg for premise in rec.get("premises", []) for seg in premise]

        for seg in anchors:
            if isinstance(seg, str) or "core" in seg:
                continue
            identifier = node_id(seg)
            if identifier is None:
                continue
            labels.setdefault(identifier, node_label(seg))
            entries = pages.setdefault(identifier, {}).setdefault(heading, [])
            if line not in entries:
                entries.append(line)

    for identifier, sections in pages.items():
        path = out_dir / f"{identifier}.md"
        text = [f"# [{labels[identifier]}]({wikidata_url(identifier)})", ""]
        for heading in ("Deductions", "Contradictions", "Derivations"):
            if heading not in sections:
                continue
            text.append(f"## {heading}")
            text.append("")
            text.extend(sections[heading])
            text.append("")
        path.write_text("\n".join(text), encoding="utf-8")

    return len(pages)


def convert_text(records: list, out_path: Path) -> int:
    with out_path.open("w", encoding="utf-8") as out:
        for rec in records:
            premises = ", ".join(render_plain(p) for p in rec.get("premises", []))
            out.write(f"{premises} => {render_plain(rec.get('conclusion', []))}\n")
    return len(records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="JSON Lines file written by .run-export")
    parser.add_argument("--format", choices=("md", "text"), default="md")
    parser.add_argument("--out", type=Path, required=True,
                        help="output directory (md) or file (text)")
    args = parser.parse_args()

    records = []
    with args.input.open(encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as exc:
                print(f"{args.input}:{number}: {exc}", file=sys.stderr)
                return 1

    if args.format == "md":
        pages = convert_md(records, args.out)
        print(f"{len(records)} derivations -> {pages} pages in {args.out}")
    else:
        written = convert_text(records, args.out)
        print(f"{written} derivations -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
