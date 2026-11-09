#!/usr/bin/env bash
# The pure-discovery curriculum from scratch, as run on 2026-11-07 to produce
# the installed cpp/ai_weights.json (1v7 stage, best selection checkpoint).
#
#   cpp/test/curriculum.sh [OUT_DIR]
#   LATE_FLAGS="--warden-frac 0.4 ..." cpp/test/curriculum.sh [OUT_DIR]
#
# LATE_FLAGS are extra w2arena flags for the full-map stages (s4, s5), e.g. a
# league of training opponents.
# Run from the repository root with the headless trainer built in
# cpp/build-train (see AI-DESIGN.md §7). About 3 hours on 72 threads. Each
# stage starts from the previous stage's final weights; the last step compares
# the candidates against cpp/ai_weights.json on the development seeds.
set -uo pipefail
if [[ ! -f original/TERRAIN0/ARMYTYPE.DAT ]]; then
  echo "curriculum.sh: run from the repository root with the game files in original/." >&2
  exit 1
fi
A=cpp/build-train/w2arena
D=${1:-runs/curriculum-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$D"
git rev-parse HEAD > "$D/commit.txt" 2>/dev/null || true
common="--solo-learner --report-seeds 0 --eval-every 10 --save-every 20"

# stage NAME INIT ITERATIONS GAMES TURNS SIDES CROP RANDOM_MAPS LOCAL_WEIGHT LR TEMP SEED_OFFSET
stage() {
  local name=$1 init=$2; shift 2
  local it=$1 gm=$2 tu=$3 si=$4 cr=$5 ra=$6 lw=$7 lr=$8 te=$9 so=${10} more=${11:-}
  mkdir -p "$D/$name"
  local extra=""
  if [[ $init == scratch ]]; then extra="--from-scratch"; else cp "$init" "$D/$name/weights.json"; fi
  $A $common $extra --iterations "$it" --games-per-iter "$gm" --turns "$tu" --eval-turns "$tu" --select-seeds 32 \
     --active-sides "$si" --crop-margin "$cr" --random-map-frac "$ra" --local-weight "$lw" --lr "$lr" \
     --temp-start "$te" --seed-offset "$so" --out "$D/$name/weights.json" --log "$D/$name/log.csv" $more \
     > "$D/$name/out.txt" 2>&1
  echo "$(date +%H:%M) $name: $(grep Kept "$D/$name/out.txt")"
}
stage s1_1v1 scratch                        100 288 28 2 14 0.50 1.5 4.0e-3 1.0  400000
stage s2_1v3 "$D/s1_1v1/weights.final.json"  80 384 34 4 14 0.50 1.5 3.5e-3 1.0  410000
stage s3_1v5 "$D/s2_1v3/weights.final.json"  48 432 38 6 14 0.35 1.0 1.5e-3 1.0  420000
stage s4_1v7 "$D/s3_1v5/weights.final.json"  80 432 40 8 0  0.15 0.0 2.0e-3 1.0  430000 "${LATE_FLAGS:-}"
stage s5_1v7 "$D/s4_1v7/weights.final.json"  96 432 40 8 0  0.05 0.0 1.5e-3 0.65 440000 "${LATE_FLAGS:-}"

for W in "$D/s4_1v7/weights.json" "$D/s4_1v7/weights.final.json" "$D/s5_1v7/weights.json"; do
  name=$(basename "$(dirname "$W")")_$(basename "$W" .json)
  $A --out cpp/ai_weights.json --compare "$W" --dev-seeds 256 --breakdown > "$D/compare_$name.txt" 2>&1
  echo "$(date +%H:%M) $name vs installed (Classic, all presets): $(grep paired "$D/compare_$name.txt")"
  # the human-style benchmark: total war against garrisoning Wardens
  $A --out cpp/ai_weights.json --compare "$W" --dev-seeds 128 --eval-preset 0 --eval-opponent warden \
     > "$D/compare_${name}_warden.txt" 2>&1
  echo "$(date +%H:%M) $name vs installed (Warden, total war): $(grep paired "$D/compare_${name}_warden.txt")"
done
