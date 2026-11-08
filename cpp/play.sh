#!/bin/sh
# bazel run //cpp:play [-- game options]: the game, started in the
# repository root, where it finds original/ and pre-rendered-sound/ and keeps
# its generated worlds.
set -eu
game=$(pwd)/$1
shift
cd "${BUILD_WORKSPACE_DIRECTORY:?run me with bazel run //cpp:play}"
exec "$game" "$@"
