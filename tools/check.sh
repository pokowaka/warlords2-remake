#!/bin/sh
# Every gate a C++ change must pass, in one go (the refactor's rule: each
# commit passes all of them). From anywhere in the repository:
#
#   sh tools/check.sh           # build, unit tests, golden regressions, and clang-tidy
#   sh tools/check.sh --quick   # build, unit tests and golden regressions only
#
# Needs bazel.
set -eu
cd "$(git rev-parse --show-toplevel)"

step() { printf '\n== %s\n' "$*"; }

step "build and test (bazel test //cpp/...)"
bazel test //cpp/...

if [ "${1:-}" = "--quick" ]; then
  exit 0
fi

step "clang-tidy (tools/tidy.sh)"
sh tools/tidy.sh

step "all gates passed"
