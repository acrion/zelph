#!/usr/bin/env python3
"""Emit a Zelph HF v2 manifest and shard tree from a .bin plus .index.json.

This is the minimal producer tool for remote partial loading:

1. Read Zelph's `.index-file` sidecar JSON.
2. Emit one shard object per section-local chunk.
3. Write a `zelph-hf-layout/v2` manifest that points at those shard objects.

It depends only on the Python standard library so it can be used without the
rest of the ITIR tooling surface.
"""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SECTION_NAMES = ("left", "right", "nameOfNode", "nodeOfName")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit a Zelph HF v2 manifest and shard files from a .bin plus .index.json."
    )
    parser.add_argument("--bin", required=True, help="Path to the local Zelph .bin file.")
    parser.add_argument("--index", required=True, help="Path to the Zelph .index-file JSON sidecar.")
    parser.add_argument("--output", required=True, help="Path to write the v2 manifest JSON.")
    parser.add_argument(
        "--hf-root",
        default="hf://datasets/acrion/zelph",
        help="Logical HF root prefix to encode into the manifest.",
    )
    parser.add_argument(
        "--artifact-name",
        default=None,
        help="Artifact name used under the HF root. Defaults to the .bin stem.",
    )
    parser.add_argument(
        "--shard-root",
        default=None,
        help="Directory where shard files will be written. Defaults to <artifact-name>_shards beside the manifest.",
    )
    parser.add_argument(
        "--node-route",
        default=None,
        help="Optional route-sidecar JSON to advertise in the manifest.",
    )
    parser.add_argument(
        "--node-route-object-path",
        default=None,
        help="Optional HF/logical path for the route sidecar. Defaults to <artifact-root>/artifact.route.json.",
    )
    return parser.parse_args()


def load_index(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    for name in SECTION_NAMES:
        if name not in data:
            raise ValueError(f"Index JSON is missing section '{name}'")
    if "header" not in data or "length" not in data["header"]:
        raise ValueError("Index JSON is missing header length information")
    return data


def range_string(offset: int, length: int) -> str:
    return f"bytes={offset}-{offset + length - 1}"


def sanitize_lang_token(lang: str) -> str:
    token = "".join(ch if (ch.isalnum() or ch in ("_", "-", ".")) else "_" for ch in lang)
    return token or "lang"


def chunk_filename(chunk_index: int, lang: str = "") -> str:
    base = f"chunk-{int(chunk_index):06d}"
    if lang:
        return f"{base}-{sanitize_lang_token(lang)}.capnp-packed"
    return f"{base}.capnp-packed"


def copy_range(src: Path, dst: Path, offset: int, length: int, chunk_size: int = 1024 * 1024) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open("rb") as src_handle, dst.open("wb") as dst_handle:
        src_handle.seek(offset)
        remaining = length
        while remaining > 0:
            n = min(chunk_size, remaining)
            chunk = src_handle.read(n)
            if not chunk:
                break
            dst_handle.write(chunk)
            remaining -= len(chunk)
        if remaining > 0:
            raise ValueError(f"Could not extract full range from {src}: {remaining} bytes short")


def build_sections(index: dict[str, Any], artifact_root: str) -> dict[str, dict[str, Any]]:
    shard_prefix = f"{artifact_root}/shards"
    sections: dict[str, dict[str, Any]] = {}
    for section_name in SECTION_NAMES:
        entries = sorted(index[section_name], key=lambda e: (int(e["chunkIndex"]), str(e.get("lang", ""))))
        chunks: list[dict[str, Any]] = []
        total_bytes = 0
        langs: set[str] = set()
        for entry in entries:
            chunk_index = int(entry["chunkIndex"])
            offset = int(entry["offset"])
            length = int(entry["length"])
            lang = str(entry.get("lang", ""))
            object_path = f"{shard_prefix}/{section_name}/{chunk_filename(chunk_index, lang)}"
            chunk = {
                "chunkIndex": chunk_index,
                "objectPath": object_path,
                "length": length,
                "sourceOffset": offset,
                "sourceRange": range_string(offset, length),
                "object": {
                    "path": object_path,
                    "mediaType": "application/octet-stream",
                    "sizeBytes": length,
                },
            }
            if "which" in entry:
                chunk["which"] = entry["which"]
            if lang:
                chunk["lang"] = lang
                langs.add(lang)
            chunks.append(chunk)
            total_bytes += length
        section: dict[str, Any] = {
            "chunkCount": len(chunks),
            "totalBytes": total_bytes,
            "chunks": chunks,
        }
        if langs:
            section["languages"] = sorted(langs)
        sections[section_name] = section
    return sections


def emit_shards(bin_path: Path, sections: dict[str, dict[str, Any]], shard_root: Path) -> None:
    for section_name, section in sections.items():
        for chunk in section["chunks"]:
            local_path = shard_root / section_name / Path(chunk["objectPath"]).name
            copy_range(bin_path, local_path, int(chunk["sourceOffset"]), int(chunk["length"]))


def build_manifest(
    bin_path: Path,
    index_path: Path,
    output_path: Path,
    hf_root: str,
    artifact_name: str,
    node_route_path: Path | None,
    node_route_object_path: str | None,
) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    index = load_index(index_path)
    artifact_root = f"{hf_root.rstrip('/')}/{artifact_name}"
    sections = build_sections(index, artifact_root)

    total_chunk_count = sum(section["chunkCount"] for section in sections.values())
    total_chunk_bytes = sum(section["totalBytes"] for section in sections.values())

    hf_objects: dict[str, Any] = {
        "manifest": {
            "path": f"{artifact_root}/{output_path.name}",
            "role": "layout-manifest",
            "mediaType": "application/json",
        },
        "index": {
            "path": f"{artifact_root}/artifact.index.json",
            "role": "offset-sidecar",
            "mediaType": "application/json",
            "sizeBytes": index_path.stat().st_size,
        },
    }
    for name, section in sections.items():
        hf_objects[name] = {
            "pathPrefix": f"{artifact_root}/shards/{name}",
            "count": section["chunkCount"],
            "role": "section-shards",
            "mediaType": "application/octet-stream",
        }
    if node_route_path is not None:
        route_path = node_route_object_path or f"{artifact_root}/artifact.route.json"
        hf_objects["nodeRouteIndex"] = {
            "path": route_path,
            "localPath": str(node_route_path),
            "role": "node-route-sidecar",
            "mediaType": "application/json",
            "sizeBytes": node_route_path.stat().st_size,
        }

    manifest = {
        "manifestVersion": "zelph-hf-layout/v2",
        "createdAtUtc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "storageMode": "multi-object-shards",
        "transport": {
            "primary": "hf-object-fetch",
            "fallback": "local-file",
        },
        "source": {
            "binPath": str(bin_path),
            "indexPath": str(index_path),
            "binSizeBytes": bin_path.stat().st_size,
            "headerLengthBytes": int(index["header"]["length"]),
            "totalChunkCount": total_chunk_count,
            "totalChunkBytes": total_chunk_bytes,
        },
        "hfObjects": hf_objects,
        "selectorModel": {
            "unit": "section-chunk",
            "supportedSections": list(SECTION_NAMES),
            "supportedOperations": [
                "header-probe",
                "selected-chunk-read",
            ],
            "unsupportedOperations": [
                "small-neighborhood-expansion",
                "reasoning-complete-query",
            ],
        },
        "sections": sections,
        "layoutPlan": {
            "isCanonical": True,
            "supportsNodeRouteIndex": node_route_path is not None,
        },
        "capabilities": {
            "headerProbe": True,
            "selectedChunkRead": True,
            "nodeRouteIndex": node_route_path is not None,
            "smallNeighborhoodExpansion": False,
            "fullReasoningSafe": False,
        },
        "cachePolicy": {
            "mode": "immutable-range-cache",
            "recommendedKeyFields": [
                "manifestVersion",
                "hfObjects",
                "sections",
            ],
            "invalidationRule": "invalidate on manifest/object identity change",
        },
        "limitations": [
            "Chunk selectors are file-local and are not guaranteed stable across regenerated .bin files.",
        ],
    }
    if node_route_path is None:
        manifest["selectorModel"]["unsupportedOperations"].insert(0, "node-route")
        manifest["limitations"].append("No node-to-chunk routing index is defined yet.")
    else:
        manifest["selectorModel"]["supportedOperations"].append("node-route")
        manifest["layoutPlan"]["nodeRoutingIndex"] = {
            "path": hf_objects["nodeRouteIndex"]["path"],
            "format": "zelph-node-route/v1",
        }

    return manifest, sections


def main() -> int:
    args = parse_args()

    bin_path = Path(args.bin).resolve()
    index_path = Path(args.index).resolve()
    output_path = Path(args.output).resolve()
    artifact_name = args.artifact_name or bin_path.stem
    node_route_path = Path(args.node_route).resolve() if args.node_route else None

    manifest, sections = build_manifest(
        bin_path=bin_path,
        index_path=index_path,
        output_path=output_path,
        hf_root=args.hf_root,
        artifact_name=artifact_name,
        node_route_path=node_route_path,
        node_route_object_path=args.node_route_object_path,
    )

    shard_root = Path(args.shard_root).resolve() if args.shard_root else output_path.parent / f"{artifact_name}_shards"
    emit_shards(bin_path, sections, shard_root)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
