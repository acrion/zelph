#!/usr/bin/env bash
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build-release/bin/zelph "${SCRIPT_DIR}/test_sharding1.zph"
build-release/bin/zelph "${SCRIPT_DIR}/test_sharding2.zph"
