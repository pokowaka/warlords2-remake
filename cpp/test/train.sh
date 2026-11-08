#!/usr/bin/env bash
# Build the headless trainer and launch a Modern-AI training run.
#
#   cpp/test/train.sh [extra w2arena flags...]
#
# Run from the repository root (original/ must hold the game files). Builds
# the trainer with Bazel, tuned for this CPU (--config=native; no SDL2), runs
# the rules suite, then trains into runs/<timestamp>/:
#   weights.json  best checkpoint by the selection set (copy to cpp/ai_weights.json)
#   log.csv       one line per iteration
#   out.txt       full console output, ending with the held-out report
# Defaults suit a large machine; any flag given here overrides them
# (later flags win). See AI-DESIGN.md, section "Continuing the work".
set -euo pipefail

if [[ ! -f original/TERRAIN0/ARMYTYPE.DAT ]]; then
  echo "train.sh: original/TERRAIN0/ARMYTYPE.DAT not found -- run from the repository root" \
       "with the game files in original/." >&2
  exit 1
fi

bazel test --config=native //cpp:headless_tests
bazel build --config=native //cpp:w2arena

RUN="runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
git rev-parse HEAD > "$RUN/commit.txt" 2>/dev/null || true
echo "train.sh: writing to $RUN"

bazel-bin/cpp/w2arena \
  --from-scratch \
  --iterations 400 \
  --games-per-iter 192 \
  --turns 40 \
  --eval-every 10 \
  --select-seeds 24 \
  --report-seeds 64 \
  --classic-frac 0.3 \
  --out "$RUN/weights.json" \
  --log "$RUN/log.csv" \
  "$@" 2>&1 | tee "$RUN/out.txt"
