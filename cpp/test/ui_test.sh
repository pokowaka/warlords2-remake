#!/bin/sh
# The front end's safety net: plays cpp/test/ui/dialogs.txt headless (SDL's
# dummy video and audio drivers, the software renderer) and checks that
# every screenshot is the same as when recorded (cpp/test/golden/ui.sha256).
#
#   bazel test //cpp:ui_test          # check
#   bazel run //cpp:ui_update         # record anew, on purpose
#   bazel run //cpp:ui_update -- DIR  # also keep the screenshots in DIR
#
# Arguments: warlords2 script golden-file [--update [DIR]]. Run from the
# directory holding original/. The player's own settings are never touched:
# W2_USER_DIR points the game at a scratch folder.
set -eu

game=$1
script=$2
golden=$3
update=${4:-}
keep=${5:-}

out=${TEST_TMPDIR:-${TMPDIR:-/tmp}}/w2ui.$$
mkdir -p "$out/shots" "$out/user"
trap 'rm -rf "$out"' EXIT
sed "s|SHOTDIR|$out/shots|g" "$script" > "$out/script.txt"

W2_USER_DIR="$out/user" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  SDL_RENDER_DRIVER=software SDL_RENDER_VSYNC=0 \
  "$game" --window --scenario ERYTHEA --seed 1 --script "$out/script.txt" > "$out/stdout.txt" 2>&1 || {
  echo "the game failed:"
  cat "$out/stdout.txt"
  exit 1
}

if command -v sha256sum > /dev/null; then sum="sha256sum"; else sum="shasum -a 256"; fi
(cd "$out/shots" && for f in $(ls | LC_ALL=C sort); do $sum "$f"; done) > "$out/ui.sha256"
grep -E '^(turn|  )' "$out/stdout.txt" >> "$out/ui.sha256" || true

if [ "$update" = "--update" ]; then
  golden=${BUILD_WORKSPACE_DIRECTORY:+$BUILD_WORKSPACE_DIRECTORY/}$golden
  cp "$out/ui.sha256" "$golden"
  [ -n "$keep" ] && mkdir -p "$keep" && cp "$out"/shots/* "$keep/"
  echo "recorded $(grep -c bmp "$golden") screens into $golden"
  exit 0
fi

if cmp -s "$golden" "$out/ui.sha256"; then
  echo "ui: all $(grep -c bmp "$golden") screens the same"
else
  echo "DIFF  ui.sha256"
  diff "$golden" "$out/ui.sha256" || true
  exit 1
fi
