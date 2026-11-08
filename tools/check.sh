#!/bin/sh
# Every gate a C++ change must pass, in one go (the refactor's rule: each
# commit passes all of them). From anywhere in the repository:
#
#   sh tools/check.sh           # build, tests, golden files, the Lua and
#                               # the other ports' worlds
#   sh tools/check.sh --quick   # build, tests and golden files only
#
# Needs bazel; the full run also luajit and node.
set -eu
cd "$(git rev-parse --show-toplevel)"

step() { printf '\n== %s\n' "$*"; }

step "build and test (bazel test //cpp/...)"
bazel test //cpp/...

if [ "${1:-}" = "--quick" ]; then
  exit 0
fi

step "the same games as the Lua remake (compare.sh)"
sh cpp/test/compare.sh

step "the same worlds in all three ports (scengen_compare.sh)"
sh cpp/test/scengen_compare.sh 1 4

step "all gates passed"
