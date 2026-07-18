#!/usr/bin/env python3
"""Emit a Zelph HF v2 manifest and shard tree from a .bin plus .index.json.

This is the minimal producer tool for remote partial loading:

1. Read Zelph's `.index-file` sidecar JSON.
2. Emit one shard object per section-local chunk.
3. Write a `zelph-hf-layout/v2` manifest that points at those shard objects.

The manifest advertises all object paths under `<hf-root>/<artifact-name>/`.
To keep publication trivial, the tool writes an upload-ready artifact tree:
the directory containing the manifest (`--output`) also receives a copy of
the offset index under its advertised name (`artifact.index.json`) and the
shard tree under `shards/`:

    <output-dir>/
      <manifest-name>.hf-v2.json
      artifact.index.json
      shards/{left,right,nameOfNode,nodeOfName}/chunk-*.capnp-packed

Uploading `<output-dir>` to the repo as `<artifact-name>` therefore publishes
exactly the layout the manifest advertises; the tool prints the matching
`hf upload` command on success.

`source.binPath` is advertised as `<hf-root>/<bin filename>` by default, so
that pure-remote loads can fetch the .bin header without passing `source-bin=`.
This assumes the source `.bin` is published at the repo root; override with
`--bin-object-path` if it lives elsewhere. Fully local loads can always pass
`source-bin=<local .bin>` instead.

It depends only on the Python standard library so it can be used without the
rest of the ITIR tooling surface.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SECTION_NAMES = ("left", "right", "nameOfNode", "nodeOfName")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit a Zelph HF v2 manifest and shard files from a .bin plus .index.json."
    )
    parser.add_argument(
        "--bin", required=True, help="Path to the local Zelph .bin file."
    )
    parser.add_argument(
        "--index", required=True, help="Path to the Zelph .index-file JSON sidecar."
    )
    parser.add_argument(
        "--output",
        required=True,
        help=(
            "Path to write the v2 manifest JSON. The containing directory becomes the "
            "upload-ready artifact tree (index copy and shards are placed next to the "
            "manifest), so a path like '<somewhere>/<artifact-name>/<artifact-name>.hf-v2.json' "
            "is recommended."
        ),
    )
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
        "--bin-object-path",
        default=None,
        help=(
            "Logical path advertised as source.binPath in the manifest; pure-remote consumers "
            "fetch the .bin header from this location. Defaults to '<hf-root>/<bin filename>', "
            "matching a source .bin published at the repo root."
        ),
    )
    parser.add_argument(
        "--shard-root",
        default=None,
        help=(
            "Directory where shard files will be written. Defaults to '<output-dir>/shards', "
            "which mirrors the layout advertised in the manifest. Overriding this breaks the "
            "mirror and requires manual path mapping at upload time."
        ),
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
    token = "".join(
        ch if (ch.isalnum() or ch in ("_", "-", ".")) else "_" for ch in lang
    )
    return token or "lang"


def chunk_filename(chunk_index: int, lang: str = "") -> str:
    base = f"chunk-{int(chunk_index):06d}"
    if lang:
        return f"{base}-{sanitize_lang_token(lang)}.capnp-packed"
    return f"{base}.capnp-packed"


def copy_range(
    src: Path, dst: Path, offset: int, length: int, chunk_size: int = 1024 * 1024
) -> None:
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
            raise ValueError(
                f"Could not extract full range from {src}: {remaining} bytes short"
            )


def build_sections(
    index: dict[str, Any], artifact_root: str
) -> dict[str, dict[str, Any]]:
    shard_prefix = f"{artifact_root}/shards"
    sections: dict[str, dict[str, Any]] = {}
    for section_name in SECTION_NAMES:
        entries = sorted(
            index[section_name],
            key=lambda e: (int(e["chunkIndex"]), str(e.get("lang", ""))),
        )
        chunks: list[dict[str, Any]] = []
        total_bytes = 0
        langs: set[str] = set()
        for entry in entries:
            chunk_index = int(entry["chunkIndex"])
            offset = int(entry["offset"])
            length = int(entry["length"])
            lang = str(entry.get("lang", ""))
            object_path = (
                f"{shard_prefix}/{section_name}/{chunk_filename(chunk_index, lang)}"
            )
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


def emit_shards(
    bin_path: Path, sections: dict[str, dict[str, Any]], shard_root: Path
) -> None:
    for section_name, section in sections.items():
        for chunk in section["chunks"]:
            local_path = shard_root / section_name / Path(chunk["objectPath"]).name
            copy_range(
                bin_path, local_path, int(chunk["sourceOffset"]), int(chunk["length"])
            )


def build_manifest(
    bin_path: Path,
    index_path: Path,
    output_path: Path,
    hf_root: str,
    artifact_name: str,
    bin_object_path: str,
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
        "createdAtUtc": datetime.now(timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
        "storageMode": "multi-object-shards",
        "transport": {
            "primary": "hf-object-fetch",
            "fallback": "local-file",
        },
        "source": {
            "binPath": bin_object_path,
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


def hf_upload_command(
    hf_root: str, artifact_name: str, artifact_dir: Path
) -> str | None:
    """Derive the `hf upload` command that publishes the artifact tree under the advertised paths.

    Returns None if the HF root does not follow the `hf://<kind>/<owner>/<repo>` pattern.
    """
    prefix = "hf://"
    if not hf_root.startswith(prefix):
        return None
    parts = hf_root[len(prefix) :].strip("/").split("/")
    if len(parts) != 3:
        return None
    kind, owner, repo = parts
    repo_types = {"datasets": "dataset", "models": "model", "spaces": "space"}
    if kind not in repo_types:
        return None
    return f"hf upload {owner}/{repo} {artifact_dir} {artifact_name} --repo-type {repo_types[kind]}"


def main() -> int:
    args = parse_args()

    bin_path = Path(args.bin).resolve()
    index_path = Path(args.index).resolve()
    output_path = Path(args.output).resolve()
    artifact_name = args.artifact_name or bin_path.stem
    bin_object_path = (
        args.bin_object_path or f"{args.hf_root.rstrip('/')}/{bin_path.name}"
    )
    node_route_path = Path(args.node_route).resolve() if args.node_route else None

    manifest, sections = build_manifest(
        bin_path=bin_path,
        index_path=index_path,
        output_path=output_path,
        hf_root=args.hf_root,
        artifact_name=artifact_name,
        bin_object_path=bin_object_path,
        node_route_path=node_route_path,
        node_route_object_path=args.node_route_object_path,
    )

    # The directory containing the manifest is the local artifact root. Everything the
    # manifest advertises below `<hf-root>/<artifact-name>/` must exist inside this
    # directory under the same relative paths, so that uploading the directory as
    # `<artifact-name>` publishes a layout in which all advertised object paths resolve.
    artifact_dir = output_path.parent
    artifact_dir.mkdir(parents=True, exist_ok=True)

    canonical_shard_root = artifact_dir / "shards"
    if args.shard_root:
        shard_root = Path(args.shard_root).resolve()
        if shard_root != canonical_shard_root:
            print(
                f"WARNING: --shard-root '{shard_root}' does not mirror the advertised layout "
                f"('{canonical_shard_root}'). The local tree will NOT be upload-ready; when "
                f"uploading, you must map the shard directory to '{artifact_name}/shards' manually.",
                file=sys.stderr,
            )
    else:
        shard_root = canonical_shard_root
    emit_shards(bin_path, sections, shard_root)

    # Place a copy of the offset index under its advertised name next to the manifest.
    index_copy = artifact_dir / "artifact.index.json"
    if index_path != index_copy:
        shutil.copyfile(index_path, index_copy)

    # Place the route sidecar (if any) under its advertised default name as well.
    if node_route_path is not None:
        if args.node_route_object_path is None:
            route_copy = artifact_dir / "artifact.route.json"
            if node_route_path != route_copy:
                shutil.copyfile(node_route_path, route_copy)
        else:
            print(
                "WARNING: --node-route-object-path is set; place the route sidecar manually so "
                "that its repo path matches the advertised path.",
                file=sys.stderr,
            )

    with output_path.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")

    print(f"Upload-ready artifact tree written to {artifact_dir}")
    print(f"  manifest: {output_path.name}")
    print(f"  index:    {index_copy.name}")
    print(f"  shards:   {shard_root}")
    print(f"  header:   source.binPath = {bin_object_path}")
    upload_command = hf_upload_command(args.hf_root, artifact_name, artifact_dir)
    if upload_command is not None:
        print("Publish with:")
        print(f"  {upload_command}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
