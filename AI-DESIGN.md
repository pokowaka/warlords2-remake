# Modern AI: design, training, and how to continue (`AI-DESIGN.md`)

This document is the handoff for the learnable "Modern AI" on the `ai` branch:
what it is, how it is trained and measured, what has been learned so far, and
exactly how to continue training on a larger machine.

> [!IMPORTANT]
> **Status (2026-11-05).** The pipeline is sound and reproducible: the game
> rules the AI plays under, its inputs and outputs, the objective, the PPO
> trainer and the evaluation protocol have all been reviewed and fixed (see
> §9). **The AI is not yet strong.** Against the 1993 Classic AI in mixed
> 4-vs-4 games it holds **19.8 % ± 8.4 % of the two camps' cities at turn 40**
> on 48 held-out games (13 % in aggregate; §6). It learns to expand in self-play but settles
> into slow, defensive play. Making it competitive is the open work, and the
> next step is more compute and the experiments in §8.

---

## 1. What the Modern AI is

Two computer players coexist, selected per side by `Side::classicAi`:

| | Classic AI | Modern AI |
|---|---|---|
| Code | `cpp/src/warlords/ai.cpp` and `ai/{cities,groups,moves,heroes,diplomacy}.cpp` | `cpp/src/warlords/ai/modern_ai.cpp`, `ml_policy.cpp`, `belief.cpp` |
| Origin | Reverse-engineered 1993 expert system (bit-compatible with the Lua remake) | Learned policy (PPO self-play) |
| Information | Reads the whole map (ignores Hidden Map for itself) | Fair information: sees only what a human would (§3) |
| Selected by | `--classic-ai`, Game › Classic AI | default; `--modern-ai`, Game › Modern AI |

The game also has an optional modern UI (`--modern-ui`: Parley, envoy
missives, F10 AI Brain inspector). It is independent of which AI plays.

## 2. How a Modern AI turn works

`modern::playTurn` (`modern_ai.cpp`) runs:

1. **Belief update** (`belief::update`): visible cities, sites and items, observed
   enemy stacks, threat maps, border strengths and fog frontiers.
2. **Diplomacy** (wave 0 only, when Diplomacy is on). For each known rival,
   DiploNet picks a stance (Ally / Peace / Uneasy / War) plus two yes/no
   sub-decisions: offer gold to end a war, and ask a friend to join our war.
3. **Military waves** (up to 6). For each group of the side's armies sharing a
   location, rules code lists candidate **targets**: hold, reinforce an own city,
   neutral city, enemy city, enemy stack, ruin/sage, temple/item, fog frontier.
   Each target is combined with candidate **coalition sizes** (1, 2, 4, N−1, N,
   fast prefix). MissionNet scores every (target, size) pair. One is sampled
   from a softmax, plus a yes/no "stage nearby first" for attacks, and executed
   with the normal movement and combat rules. Stacks that win or reveal fog with
   moves left go again in the next wave.
4. **Economy**. EconNet scores, as separate categorical choices:
   - keep vs disband (only offered when upkeep exceeds income);
   - per city, stop vs each production slot;
   - buy a production slot or hold gold;
   - per city, the vectoring destination;
   - after a capture: occupy / pillage / sack / raze.

**Rules, not learned:** candidate generation, pathing, combat estimation, the
hero-arrival city (most threatened own city), and the treaty rule (an attack
needing a declaration happens only while DiploNet is not proposing peace to that
side; `attackable()` / `mustDeclare`).

**Sampling in real games.** Real games sample the policy too, at
`DEPLOY_TEMPERATURE` = 0.5. The generator is seeded from the game's dice state,
the turn and the side, and never advances the game's RNG, so games stay
reproducible. Training anneals its temperature to the same value. Greedy
argmax play was measurably worse: a spread-out policy's mode is not its typical
behaviour.

### The network (`ml_policy.hpp`)
Four residual MLPs, `y = W_skip·x + W3·tanh(W2·tanh(W1·x + b1) + b2) + b3`,
6,729 parameters in total, with hand-written forward and backward passes
(no ML library):

| Head | Input | Output |
|---|---|---|
| DiploNet 42→24→12→6 | context 24 + rival 18 | 4 stance logits, tribute logit, joint-war logit |
| MissionNet 61→32→16→2 | context 24 + stack 16 + target 21 | utility, stage-near logit |
| EconNet 45→24→12→1 | context 24 + econ 21 | score (one-hot `[0..6]` selects the decision type; `[20]` flags the current choice) |
| ValueNet 24→24→12→1 | context 24 | V(s), linear, in empire-score units |

Context is 14 global fair-information features plus 10 personality channels,
which are held at 0 for now.

**Multiplicity prior.** Option families have very different sizes (40 fog
frontiers vs 1 hold). The policy logit is therefore `z_i = u_i − ln N_family(i)`;
sampling is `softmax(z/T)`. Every family starts with equal mass, and the prior
is part of the policy rather than the temperature. A KL pull toward this prior
is the entropy regulariser.

Weights live in `cpp/ai_weights.json` (format version 3: `generation` is the
training iteration kept, `selectionCityShare` its selection score). Changing
any input or output size invalidates them; the game then prints a warning and
plays with random weights.

## 3. Fair-information rules
- Pathing obeys Hidden Map for Modern AI sides exactly as for humans
  (`move::prepare`).
- Enemy stacks: only flag count and top unit, unless View Enemies is on. Fuller
  knowledge comes only from battles fought (`recordBattleObservation`).
- Combat estimates (`estimateCombat`) build defenders from those observations
  and run 8 simulated battles with the game's RNG saved and restored.
- The attitude score fed to DiploNet counts only visible cities.
- Known remaining public-knowledge assumptions: capital identity, diplomatic
  scores (shown in the diplomatic report), and neutral garrisons standing on a
  city's top-left tile.
- The empire score (§4) reads the true state, but only as a training reward,
  never as a policy input.

## 4. Training objective (`cpp/test/arena.cpp`)

**Empire score** (`computeEmpireScore`, `ml_policy.cpp`):

    S = 2·cityShare + 0.5·incomeShare + 0.25·armyShare (+ ≤0.15 recon under Hidden Map),  S = 0 when eliminated

Shares are of all standing cities, total income, and all sides' army strength.
S is the **one deliberate prior**: a hand-chosen definition of "doing well".

**Reward** for a side's turn t is `S(start of its next turn) − S(start of turn t)`,
so losses during rivals' turns count. A win adds +1 and elimination −0.5.
Training games stop after `--turns`. That cut-off is a time limit, not the end
of the game, so the critic bootstraps there. The objective is therefore the
discounted growth of S with no fixed horizon (γ = 0.97, so about 33 turns
ahead).

## 5. Trainer and evaluation

**PPO.** Each iteration:
1. Play `--games-per-iter` games in parallel (one thread per game). The opponent
   mix:
   - half the games are mirror matches (every side is the learner and recorded);
   - half are league matches (half the sides play a hall-of-fame snapshot, taken
     every 10 iterations, up to 12);
   - `--classic-frac` of all games instead pit half the sides against the
     Classic AI.
2. Compute GAE per side (λ = `--lambda`, default 0.95). Mission decisions get the
   turn's advantage plus `--local-weight` × their own score change while they
   acted. That term is centred on the turn's mean if the weight is ≤ 1, and
   taken as-is above 1.
3. Normalise advantages per head, then run 4 epochs × 8 minibatches of turns.
   Each head's loss is a mean over its own decisions; the critic loss is
   0.5·(V − G)² averaged over turns with weight 0.5. Gradients are clipped to a
   global norm of 0.5. Adam: learning rate 3e-3 → 3e-4. Epochs stop early if the
   approximate KL exceeds 0.03.
4. Temperature anneals 1.0 → 0.5 (`DEPLOY_TEMPERATURE`) over the run.

**Evaluation (never used for training).** Mixed 4-vs-4 games, Modern vs
Classic, cut off at `--eval-turns`. Each seed is played twice with the side
assignment swapped. The figure of merit is the Modern camp's share of the
cities held by both camps (the mean over games, with a 95 % interval), plus how
often the largest side is Modern. There are two disjoint seed sets, both
disjoint from training seeds:
- **selection** (`--select-seeds`, base 700000): every `--eval-every`
  iterations. The best score is saved to `--out`.
- **report** (`--report-seeds`, base 800000): run once at the end on the kept
  checkpoint, and by `--benchmark-only`. Never used for any decision. **Quote
  only this number.**

Scenarios and presets rotate over ERYTHEA, ISLADIA, DRAGON, TUTORIA, SORCERY
and HADESHA, × {Beginner-Open, Interm-Diplo, Adv-Hidden, Fog-TotalWar}.

## 6. Results so far

All runs: 32 games per iteration, 40-turn games, 16-core Mac, two runs sharing
the CPU. Selection scores used 8 seeds (16 games), so each figure is ±5–12 %.

| Run | Setup | Selection city share (iterations 10…100) | Best |
|---|---|---|---|
| A | pure self-play | 9, 8, 11, 18, 14, 9 % … (stopped at 68) | 18.2 % @40 |
| B | `--classic-frac 0.3` | 3, 12, 17, 12, 10, 14, 13, 14 % … | 16.5 % @30 |
| C | B + `--local-weight 5 --lambda 0.9` | 9, 15, 14, 10, 12 % … (stopped at 58) | 15.1 % @20 |
| B report | kept checkpoint (iteration 30), 48 held-out games | **19.8 % ± 8.4 %** mean share; 305 vs 2,024 cities in aggregate (13 %); Modern largest side in 5/48 | installed as `cpp/ai_weights.json` |

The mean share is above the aggregate because a few games went well. Both
figures are worth tracking.

For reference, the weights shipped before this rework claimed 35 %, but that
was on 8 games at 22 turns, selected on the same games it was reported on. It
is not comparable.

**What the diagnostics show** (`w2aidiag`, §7):
- In self-play, cities per side rise from 2.4 to about 7–8 and almost every
  neutral city is taken, so the trainer clearly learns.
- Against the Classic AI, the policy:
  - builds mostly the cheapest units (Light Infantry, Scouts);
  - keeps most armies in its cities and hoards gold;
  - spends most non-hold moves on "Reinforce" (shuttling armies between its own
    cities): in one 30-turn game, 1,004 reinforce moves against 442
    neutral-capture and 144 siege moves.
- The Classic AI expands from 2 to 30–40 cities in the same games.
- Training signals: KL per update is about 0.001 (far below the 0.03 limit);
  entropy falls mainly through the temperature schedule. The policy moves
  slowly because the per-turn reward in an 8-side game is dominated by rivals'
  turns.
- Neither Classic opponents (B) nor stronger per-stack credit (C) clearly beat
  pure self-play at this scale. Their intervals overlap.

**Throughput.** One 40-turn game costs about 1.5–5 CPU-seconds. It grows as
armies accumulate, because each AI turn scores every (target × size) option and
runs 8 simulated battles per combat option. Per iteration on the Mac: about 4 s
early to 30–60 s later. The tile-index optimisation (§9) cut AI time by about
1.5×. Memory is small: 16 games use about 100 MB.

## 7. Continuing the work on a large Linux machine

### Setup
```bash
# Ubuntu/Debian: compiler, CMake, Ninja (SDL2 is NOT needed for training)
sudo apt install build-essential cmake ninja-build git
git clone <repo> warlords2-remake && cd warlords2-remake && git checkout ai
# The original game files must be in original/ (same layout as on the dev machine):
ls original/TERRAIN0/ARMYTYPE.DAT
```

### Train
```bash
cpp/test/train.sh                          # defaults: 400 iterations x 192 games, 30% Classic
cpp/test/train.sh --classic-frac 0         # any w2arena flag overrides the defaults
nohup cpp/test/train.sh > /dev/null 2>&1 &  # detach; output still lands in runs/<timestamp>/
```
`train.sh` builds into `cpp/build-train` with `-DW2_BUILD_GAME=OFF -DW2_NATIVE=ON`,
runs `w2test`, and writes `runs/<timestamp>/{weights.json, log.csv, out.txt, commit.txt}`.
The trainer uses every hardware thread for games and for gradients.

### Monitor
- `tail -f runs/*/out.txt`. Each iteration prints: time, temperature, learning
  rate, cities per side in self-play, neutral cities left, normalised entropy,
  value loss, KL (and epochs run), turns collected. Selection lines appear every
  `--eval-every` iterations.
- `runs/*/log.csv` columns: `iter, seconds, temperature, lr, avg_cities,
  neutrals_left, entropy, value_loss, kl, epochs_run, decisions, select_share,
  select_ci`.
- Healthy signs: `avg_cities` rises then flattens; `select_share` trends up;
  KL stays between about 0.002 and 0.03.
- KL ≈ 0.001 means updates are too timid: raise `--lr`, or use bigger batches.
- KL repeatedly over 0.03 with `epochs_run` = 1 means too aggressive: lower
  `--lr`.

### Evaluate, inspect, install
```bash
B=cpp/build-train
$B/w2arena --benchmark-only --report-seeds 64 --eval-turns 40 --out runs/X/weights.json   # held-out report
$B/w2aidiag runs/X/weights.json ERYTHEA 800001 40          # per-side economy, mission mix, production
cp runs/X/weights.json cpp/ai_weights.json                 # install; commit with the report numbers
```
**Resuming.** Without `--from-scratch`, `w2arena` loads `--out` if it exists.
Adam state, the temperature schedule (which restarts at 1.0) and the hall of
fame are not saved. For a warm restart, pass e.g. `--lr 1e-3` and accept the
temperature restart.

### w2arena flags
| Flag | Default | Meaning |
|---|---|---|
| `--iterations N` | 300 | PPO iterations |
| `--games-per-iter G` | 48 | games per iteration (scale with cores) |
| `--turns T` | 40 | training game cut-off (time limit; critic bootstraps) |
| `--eval-every E` | 10 | selection evaluation interval |
| `--eval-turns T` | 40 | evaluation game cut-off |
| `--select-seeds S` | 12 | selection seeds (×2 games each) |
| `--report-seeds R` | 32 | held-out report seeds (×2 games each) |
| `--epochs` / `--minibatches` | 4 / 8 | PPO passes and minibatches per iteration |
| `--lr X` | 3e-3 | starting learning rate (decays linearly to 3e-4) |
| `--lambda L` | 0.95 | GAE λ |
| `--local-weight W` | 1 | weight of a stack's own score change in its advantage |
| `--classic-frac F` | 0 | fraction of training games against the Classic AI |
| `--from-scratch` | off | random init instead of loading `--out` |
| `--out PATH` / `--log CSV` | `cpp/ai_weights.json` / none | checkpoint and log |
| `--benchmark-only` | off | only run the held-out report on `--out` |
| `--data DIR` | `original` | game files |

## 8. Recommended next experiments (in order)

1. **Scale the batch first.** 192–384 games per iteration instead of 32. The
   main symptom is noisy, timid updates (KL ≈ 0.001), which bigger batches
   address directly. Then run long: 400+ iterations.
2. **Sweep the opponent mix and credit** on the big batch: `--classic-frac`
   {0, 0.3, 0.5} × `--local-weight` {1, 5}. Several runs fit side by side on
   96 cores (give each a separate `runs/` directory, e.g. by launching
   `train.sh` a second later).
3. **Fix the action design that enables the defensive equilibrium.** Reinforce
   currently competes with every attack for every unit, every wave. Options:
   - cap reinforce moves per city;
   - make reinforce a per-city economy decision instead of a per-unit mission;
   - give MissionNet a "turns since this stack last fought" feature.
4. **Per-stack value baseline.** A small critic on (context, stack features)
   would give each mission its own baseline instead of the shared turn
   advantage. This is the principled fix for 8-player credit noise.
5. **Curriculum:** 20-turn games early (cheaper, denser signal), lengthening to
   60+ later.
6. **Bigger model.** Only after 1–4. A map-wide convolutional encoder would need
   a GPU and probably PyTorch, with the C++ games feeding it data. The current
   6.7k-parameter MLPs run fine on CPU.
7. **Personalities.** The 10 conditioning channels are wired but zero. Once a
   base policy is competitive, train with card-derived traits in those channels.

## 9. Change history of this design (`ai` branch)

| Commit | Change |
|---|---|
| `2869e5e`, `da1734d` | First learnable AI and trainer (on `main`). Overclaimed: "zero priors, learns from win/loss", 35 % vs Classic. A later review found the issues below. |
| `d04ad44` | Classic UI by default (`--modern-ui` opts in); Game-menu Modern/Classic AI toggle; Classic saves stay Classic. |
| `101a05a` | Closed info leaks (attitude score, stale stack checks). Units walk onto one tile instead of teleporting (stack limit). Tribute and joint war are trained binary actions. Declarations use `mustDeclare`. Econ "current choice" input; disband only when over budget. |
| `792a2c9` | Trainer rewrite: honest empire-score objective with bootstrapping, linear critic with proper weight, minibatched PPO with KL stop, disjoint selection/report seeds with swapped sides and CIs. |
| `1302ed6` | Real games sample the policy at T = 0.5 instead of greedy argmax. |
| `8aeaab3` | ~1.5× faster AI turns (per-tile army index; flying flood only for fliers). |
| `52813b0` | Headless trainer build (`W2_BUILD_GAME=OFF`, `W2_NATIVE`), `cpp/test/train.sh`, `--local-weight`, `--lambda`. |
| (handoff commit) | `w2aidiag`; the game warns when weights fail to load; run B checkpoint installed; this document. |

**Review items deliberately left open** (they do not affect training):
- `Side::classicAi` doubles as the diplomacy rule-variant flag, and it is set on
  human sides too. With Modern AI + the classic UI, a human pays hidden trust
  penalties.
- Run `sh cpp/test/compare.sh` (needs luajit) to re-confirm Classic-AI parity
  with the Lua reference after the shared `diplomacy.cpp` / `game.cpp` changes.

## 10. File map

| File | Role |
|---|---|
| `cpp/src/warlords/ai/modern_ai.cpp` | Turn engine: candidates, waves, execution, economy, diplomacy |
| `cpp/src/warlords/ai/ml_policy.{hpp,cpp}` | Network, sampling (`selectFromLogits`, `selectBinary`), context features, empire score, weights I/O |
| `cpp/src/warlords/ai/belief.{hpp,cpp}` | Fair-information belief, fog frontiers, combat estimates, `ScopedTileIndex` |
| `cpp/src/warlords/ai/personality.{hpp,cpp}` | 27 character-card profiles (UI, attitude score; future conditioning) |
| `cpp/src/warlords/diplomacy.{hpp,cpp}` | Alliances, truces, tribute, trust, `computeAttitude` |
| `cpp/test/arena.cpp` | `w2arena`: PPO trainer and Classic benchmark |
| `cpp/test/aidiag.cpp` | `w2aidiag`: behaviour diagnostic for a checkpoint |
| `cpp/test/train.sh` | Headless build and training launcher |
| `cpp/test/run.cpp` (`testModernAi`) | Unit tests: fog pathing, diplomacy, sampling, binary decisions, stack limit, engine switching |
| `cpp/ai_weights.json` | Installed checkpoint |

## 11. Playing against it
```bash
cpp/build/warlords2 --window --scenario ERYTHEA --diplomacy --hidden-map               # Modern AI, classic UI
cpp/build/warlords2 --window --modern-ui --scenario ERYTHEA --diplomacy --hidden-map   # with Parley / F10 inspector
cpp/build/warlords2 --window --classic-ai --scenario ERYTHEA                           # the 1993 AI
cpp/build/warlords2 --window --modern-ui --scenario ERYTHEA --spectate                 # watch 8 AIs, full map
cpp/build/warlords2 --weights runs/X/weights.json ...                                  # try a checkpoint
```
