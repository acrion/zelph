#!/usr/bin/env bash
# Fetch the GitHub-Actions-built zelph playground into site/play.
#
# Usage:
#   ./get-playground.sh <branch>        artifact of the latest successful
#                                       ci.yml run on <branch>
#   ./get-playground.sh release [tag]   zelph-playground.zip asset of a
#                                       GitHub release (default: latest release)
#
# Requirements: GitHub CLI (gh, authenticated) and unzip.
#
# Meant to be run after building the mkdocs site (mkdocs build / generate-mkdocs.sh),
# because the site directory is rebuilt from scratch there, wiping /play.

set -euo pipefail
cd "$(dirname "$0")"

REPO="acrion/zelph"
MODE="${1:?Usage: $0 <branch> | release [tag]}"

rm -rf site/play
mkdir -p site/play

if [[ "$MODE" == "release" ]]; then
    TAG="${2:-}"
    ZIP="site/play/zelph-playground.zip"
    if [[ -n "$TAG" ]]; then
        gh release download "$TAG" --repo "$REPO" --pattern zelph-playground.zip --output "$ZIP"
    else
        gh release download --repo "$REPO" --pattern zelph-playground.zip --output "$ZIP"
    fi
    unzip -q "$ZIP" -d site/play
    rm "$ZIP"
else
    RUN_ID=$(gh run list --repo "$REPO" --branch "$MODE" --workflow ci.yml \
                 --status success --limit 1 --json databaseId --jq '.[0].databaseId')
    if [[ -z "$RUN_ID" || "$RUN_ID" == "null" ]]; then
        echo "Error: no successful ci.yml run found on branch '$MODE'." >&2
        exit 1
    fi
    gh run download "$RUN_ID" --repo "$REPO" --name zelph-playground --dir site/play
fi

if [[ ! -f site/play/index.html || ! -f site/play/zelph.wasm ]]; then
    echo "Error: download incomplete - site/play lacks index.html or zelph.wasm." >&2
    exit 1
fi

echo "Playground is in $(pwd)/site/play"
