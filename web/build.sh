#!/bin/bash
set -euo pipefail

ROOT="${BUILD_WORKSPACE_DIRECTORY:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
cd "$ROOT"

exec ./tools/build_web.sh "$@"
