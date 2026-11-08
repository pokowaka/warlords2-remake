#!/bin/sh
# Make the same scenario-generator worlds in the C++, JavaScript and Lua
# ports and compare them byte for byte (docs/scengen.md). Run from the
# repository root, after `bazel build //cpp:w2scengen` (or point W2SCENGEN
# at another w2scengen):
#     sh cpp/test/scengen_compare.sh [first-seed count]
set -u
first=${1:-1}
count=${2:-4}
tmp=${TMPDIR:-/tmp}/w2scengen.$$
mkdir -p "$tmp/cpp" "$tmp/js" "$tmp/lua"
fail=0
for shape in continents pangaea islands; do
  ${W2SCENGEN:-bazel-bin/cpp/w2scengen} --dump "$tmp/cpp" --shape $shape --seed "$first" --count "$count" || fail=1
  node web/test/scengen_dump.js "$tmp/js" $shape "$first" "$count" || fail=1
  luajit love2d/test/scengen_dump.lua "$tmp/lua" $shape "$first" "$count" || fail=1
done
for dir in "$tmp"/cpp/*; do
  world=$(basename "$dir")
  for port in js lua; do
    if diff -rq "$dir" "$tmp/$port/$world" >/dev/null 2>&1; then
      echo "same  $world ($port)"
    else
      echo "DIFF  $world ($port)"
      diff -rq "$dir" "$tmp/$port/$world" 2>&1 | head -4
      fail=1
    fi
  done
done
rm -rf "$tmp"
exit $fail
