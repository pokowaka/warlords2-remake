#!/bin/sh
# The refactor's safety net: plays the same all-computer games and makes the
# same worlds as when the golden files were recorded, and checks that nothing
# changed (cpp/test/golden/).
#
#   bazel test //cpp:golden_test                   # check
#   bazel run //cpp:golden_update                  # record anew, on purpose
#
# What it records:
#   trace/*.txt       w2trace's turn-by-turn log of each game compare.sh plays
#                     against the Lua remake
#   scengen.sha256    every file of the scenario generator's worlds
#   classic.sha256    every file of the 1993 random-map generator's maps, plain
#                     and with fair capitals
#
# Arguments: w2trace w2scengen w2mapeval golden-dir [--update]. Run from the
# directory holding original/ (Bazel's runfiles, or the repository root).
set -eu

trace=$1
scengen=$2
mapeval=$3
golden=$4
update=${5:-}

out=${TEST_TMPDIR:-${TMPDIR:-/tmp}}/w2golden.$$
mkdir -p "$out/trace"
trap 'rm -rf "$out"' EXIT

# The games compare.sh plays, one per line: scenario seed turns [options].
while read -r spec; do
  [ -z "$spec" ] && continue
  name=$(echo "$spec" | tr ' ' '_')
  # shellcheck disable=SC2086  # the spec is the arguments
  "$trace" $spec > "$out/trace/$name.txt" 2>&1
done <<'EOF'
ERYTHEA 1 60
ISLADIA 7 60
DRAGON 3 50
HADESHA 11 50
SORCERY 5 50
TUTORIA 2 40
ERYTHEA 9 50 hidden
ISLADIA 4 50 hidden
ERYTHEA 3 40 save
ISLADIA 3 40 diplo
ERYTHEA 4 60 diplo
DRAGON 6 40 diplo hidden
RANDOM 3 40
RANDOM 8 30 diplo
RANDOM 5 25 save
EOF

# A sha256 line per file under a directory, by path relative to it.
manifest() {
  (cd "$1" && find . -type f | LC_ALL=C sort | while read -r file; do
    if command -v sha256sum > /dev/null; then sha256sum "$file"; else shasum -a 256 "$file"; fi
  done)
}

for shape in continents pangaea islands; do
  "$scengen" --dump "$out/scengen" --shape "$shape" --seed 1 --count 4 > /dev/null
done
manifest "$out/scengen" > "$out/scengen.sha256"

for generator in classic fair; do
  "$mapeval" --generator "$generator" --seeds 1-12 --export "$out/classic" > /dev/null
done
manifest "$out/classic" > "$out/classic.sha256"

if [ "$update" = "--update" ]; then
  # bazel run: into the source tree, not the runfiles
  golden=${BUILD_WORKSPACE_DIRECTORY:+$BUILD_WORKSPACE_DIRECTORY/}$golden
  rm -rf "$golden/trace"
  mkdir -p "$golden/trace"
  cp "$out"/trace/*.txt "$golden/trace/"
  cp "$out/scengen.sha256" "$out/classic.sha256" "$golden/"
  echo "recorded into $golden"
  exit 0
fi

fail=0
for file in "$golden"/trace/*.txt; do
  name=$(basename "$file")
  if ! cmp -s "$file" "$out/trace/$name"; then
    echo "DIFF  trace/$name"
    diff "$file" "$out/trace/$name" | head -6 || true
    fail=1
  fi
done
for file in scengen.sha256 classic.sha256; do
  if ! cmp -s "$golden/$file" "$out/$file"; then
    echo "DIFF  $file"
    diff "$golden/$file" "$out/$file" | head -6 || true
    fail=1
  fi
done
[ $fail -eq 0 ] && echo "golden: all the same"
exit $fail
