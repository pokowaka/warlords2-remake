#!/bin/sh
# Makes a zip of the game to share: the program, the trained AI, the game's
# files and the recorded music, with a launcher. From anywhere in the
# repository:
#
#   sh tools/package.sh
#
# Out comes bazel-bin/warlords2-<git describe>-<os>-<arch>.zip. Unpack it
# anywhere writable and run ./play.sh. SDL2 and Vorbis are linked in, so the
# program needs only the system's own libraries.
set -eu
root=$(git rev-parse --show-toplevel)
cd "$root"
bazel build -c opt //cpp:warlords2
name="warlords2-$(git describe --tags --always)-$(uname -s | tr 'A-Z' 'a-z')-$(uname -m)"
stage=$(mktemp -d)/$name
mkdir -p "$stage"
cp bazel-bin/cpp/warlords2 "$stage/"
chmod u+w "$stage/warlords2"
strip "$stage/warlords2" 2> /dev/null || true
cp cpp/ai_weights.json "$stage/"
# the game's files as committed, not the working tree's leftovers
git archive --format=tar HEAD original pre-rendered-sound | tar -x -C "$stage"
cat > "$stage/play.sh" <<'LAUNCH'
#!/bin/sh
# Starts the game from its own folder, so that original/ and
# pre-rendered-sound/ are found. Arguments go through to the game.
cd "$(dirname "$0")" || exit 1
exec ./warlords2 "$@"
LAUNCH
chmod +x "$stage/play.sh"
out="$root/bazel-bin/$name.zip"
rm -f "$out"
(cd "$(dirname "$stage")" && zip -qr "$out" "$name")
rm -rf "$(dirname "$stage")"
echo "wrote $out"
