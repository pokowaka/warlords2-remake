#!/bin/bash
set -euo pipefail

# Ensure emsdk environment is loaded if ~/.emsdk exists
if [ -f "$HOME/.emsdk/emsdk_env.sh" ] && ! command -v em++ >/dev/null 2>&1; then
    source "$HOME/.emsdk/emsdk_env.sh" >/dev/null 2>&1
fi

exec python3 "$(dirname "$0")/build_web.py" "$@"
