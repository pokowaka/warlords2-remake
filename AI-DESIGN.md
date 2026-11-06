# Modern AI: design, training, and how to continue (`AI-DESIGN.md`)

This document is the handoff for the learnable "Modern AI" on the `ai` branch:
what it is, how it is trained and measured, what has been learned so far, and
exactly how to continue training on a larger machine.

> [!IMPORTANT]
> **Status (2026-11-06).** Trained via a pure-discovery small-to-large curriculum
> (`1` Modern learner vs `K − 1` Warlord Classic AIs, `1v1 → 1v3 → 1v7`, zero
> hand-coded `if-then-else` tactical filters) on a 72-thread Linux box, the
> installed 5-head `8,066`-parameter checkpoint (`cpp/ai_weights.json`) reaches
> **32.8 % ± 5.6 % city share** on **128 held-out report games** (`1,710` vs
> `4,793` cities = 26.3 % aggregate; Modern largest side in **30 / 128**, vs the
> handoff checkpoint's **18.4 % ± 4.9 %**, `743` vs `5,720`, `13 / 128` —
> non-overlapping 95 % CIs) and **32.1 % ± 4.0 %** across **256 held-out games**
> (`3,271` vs `9,423` cities; largest side in **55 / 256** vs `22 / 256` at
> handoff; §6). It ties Classic AI `50 / 50` (`377` vs `377` cities) on cropped
> `1v1` maps and wins `54–87 %` on smaller scenarios (`SORCERY`, `DRAGON`),
> while full 8-player 80-city maps (where Classic AI reads Hidden Map
> omnisciently) still favour Classic ~`2 : 1` past turn 25 (§6, §8).

---

## 1. What the Modern AI is

Two computer players coexist, selected per side by `Side::classicAi`:

| | Classic AI | Modern AI |
|---|---|---|
| Code | `cpp/src/warlords/ai.cpp` and `ai/{cities,groups,moves,heroes,diplomacy}.cpp` | `cpp/src/warlords/ai/modern_ai.cpp`, `ml_policy.cpp`, `belief.cpp` |
| Origin | Reverse-engineered 1993 expert system (bit-compatible with the Lua remake) | Learned policy (PPO self-play & `1 vs K−1` Classic curriculum) |
| Information | Reads the whole map (ignores Hidden Map for itself) | Fair information: sees only what a human would (§3) |
| Selected by | `--classic-ai`, Game › Classic AI | default; `--modern-ai`, Game › Modern AI |

The game also has an optional modern UI (`--modern-ui`: Parley, envoy
missives, F10 AI Brain inspector). It is independent of which AI plays.

## 2. How a Modern AI turn works

`modern::playTurn` (`modern_ai.cpp`) runs:

1. **Belief update** (`belief::update`): visible cities, sites and items, observed
   enemy stacks, threat maps, border strengths and fog frontiers.
2. **Diplomacy** (whenever Diplomacy is on, once per encountered rival per turn,
   including rivals newly revealed mid-turn by scouting). For each known rival,
   DiploNet picks a stance (Ally / Peace / Uneasy / War) plus two yes/no
   sub-decisions: offer gold to end a war, and ask a friend to join our war.
3. **Military waves** (up to 6). For each group of the side's armies sharing a
   location, rules code lists every legal **target**: hold garrison, reinforce an
   own city, neutral city, enemy city, enemy stack, ruin/sage, temple/item, fog
   frontier. Each target (including `HOLD_GARRISON`) is combined with candidate
   **coalition sizes** (1, 2, 4, N−1, N, fast prefix). MissionNet scores every
   (target, size) pair with zero tactical `if-then-else` filtering. One is sampled
   from a softmax, plus a yes/no "stage nearby first" for attacks, and executed
   with the normal movement and combat rules. Stacks that win or reveal fog with
   moves left go again in the next wave.
4. **Economy**. EconNet scores, as separate categorical choices:
   - keep vs disband (`e[5] = 1`, only offered when upkeep exceeds income);
   - up to 3 empire-wide production-slot purchase rounds per turn (`e[3] = 1`,
     including slot fullness `e[16]`, strength upgrade delta `e[17]`, and legal
     slot-0 replacement in full 4-slot cities), stopping as soon as EconNet picks
     `HoldGold` (`e[0] = 1`);
   - per city, macro-action production (`Stop` vs each production slot via
     `game::setProduction`), queried whenever a city is idle or has just finished
     a unit (`c->countdown == slot.time`) so multi-turn elite builds are never
     cancelled mid-countdown;
   - per city, the vectoring destination;
   - after a capture: occupy / pillage / sack / raze.

**Rules, not learned:** legal candidate generation, pathing, combat estimation,
the hero-arrival city (most threatened own city), and the treaty rule (an attack
needing a declaration happens only while DiploNet is not proposing peace to that
side; `attackable()` / `mustDeclare`).

**Sampling in real games.** Real games sample the policy too, at
`DEPLOY_TEMPERATURE` = 0.5. The generator is seeded from the game's dice state,
the turn and the side, and never advances the game's RNG, so games stay
reproducible. Training anneals its temperature to the same value. Greedy
argmax play was measurably worse: a spread-out policy's mode is not its typical
behaviour.

### The network (`ml_policy.hpp`)
Five residual MLPs, `y = W_skip·x + W3·tanh(W2·tanh(W1·x + b1) + b2) + b3`,
**8,066 parameters** in total, with hand-written forward and backward passes
(no ML library):

| Head | Input | Output |
|---|---|---|
| DiploNet 42→24→12→6 | context 24 + rival 18 | 4 stance logits, tribute logit, joint-war logit |
| MissionNet 61→32→16→2 | context 24 + stack 16 + target 21 | utility, stage-near logit |
| EconNet 45→24→12→1 | context 24 + econ 21 | score (disjoint `[0..6]` selects decision type; `[20]` flags current choice) |
| ValueNet 24→24→12→1 | context 24 | V(s), empire-level critic in empire-score units |
| StackValueNet 40→24→12→1 | context 24 + stack 16 | V_stack(s), pre-decision pool critic for multi-turn per-stack local returns |

Context is 14 global fair-information features plus 10 personality channels,
which are held at 0 for now.

**Multiplicity prior.** Option families have very different sizes (40 fog
frontiers vs 1 hold). The policy logit at temperature `T` is therefore
`z_i = u_i / T − ln N_family(i)`, sampled via `softmax(z)`. Subtracting
`ln N_family(i)` *outside* `1/T` ensures every equal-utility family receives equal
probability mass at any temperature (fixing an earlier `(u_i − ln N)/T` bug that
penalised multi-option families by `1/N²` at `T = 0.5`). A KL pull toward the
uniform-family prior `q_i ∝ 1 / N_family(i)` is the entropy regulariser.

Weights live in `cpp/ai_weights.json` (format version 4; legacy version 3 files
with `6,729` parameters still load cleanly with `StackValueNet` zero-initialised).

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

Shares are of all standing cities, total income, and Lanchester `strength²`
across all sides' armies (including vectored units in `transit`), with `recon`
measured over the active map box (`activeX0..activeX1 × activeY0..activeY1`).
S is the **one deliberate prior**: a hand-chosen definition of "doing well".

**Reward** for a side's turn t is `S(start of its next turn) − S(start of turn t)`,
so losses during rivals' turns count. A win adds +1 and elimination −0.5.
Training games stop after `--turns`. That cut-off is a time limit, not the end
of the game, so the critic bootstraps there. The objective is therefore the
discounted growth of S with no fixed horizon (γ = 0.97, so about 33 turns
ahead).

## 5. Trainer, small-to-large curriculum, and evaluation

**PPO.** Each iteration:
1. Play `--games-per-iter` games in parallel (one thread per game). Two opponent
   modes are supported:
   - **`--solo-learner` (`1 vs K − 1` Classic AI):** exactly 1 active seat
     (rotated uniformly via `seed % K`) is the recorded Modern AI learner; all
     `K − 1` remaining active seats run the 1993 Classic AI (`level = 2`,
     Warlord). Combined with `--active-sides K --crop-margin M --random-map-frac F`,
     this supports a small-to-large curriculum (`1v1` cropped maps → `1v3`
     cropped maps → `1v7` full 8-side scenarios, plus thread-safe in-memory
     random maps via `scn::loadFromFiles`).
   - **Self-play / league (`--classic-frac F`):** half mirror matches, half
     hall-of-fame league matches, with fraction `F` against the Classic AI.
2. Compute GAE per side (λ = `--lambda`, default 0.95). Mission decisions get the
   turn's advantage plus `--local-weight` × `(G_pool − V_stack(s))`, where
   `G_pool` sums multi-turn (`γ_stack = 0.90`) immediate score changes over the
   pre-decision group's permanent `Game::pool` `Army*` addresses (`uintptr_t`,
   avoiding recycled `Army::slot` aliasing when units die). Early curriculum
   stages (`1v1`, `1v3`) use `--local-weight 1.5–2.0` for sharp per-stack
   offensive credit; full 8-side `1v7` polishing uses `--local-weight 0.0` so
   rival-turn counter-captures are weighted cleanly via empire-score GAE.
3. Normalise advantages per head, then run 4 epochs × 8 minibatches of turns.
   Each head's loss is a mean over its own decisions; `ValueNet` and
   `StackValueNet` MSE losses each carry weight 0.5. Gradients are clipped to a
   global norm of 0.5. Adam decays linearly from `--lr` to `3e-4`. Epochs stop
   early if the approximate KL exceeds 0.03.
4. Temperature anneals `--temp-start` (default 1.0) → 0.5 (`DEPLOY_TEMPERATURE`).

**Evaluation (never used for training).** Mixed 4-vs-4 games on full maps (or
`1 vs K − 1` during cropped-map selection when `--solo-learner` is set), Modern
vs Classic, cut off at `--eval-turns`. On full maps each seed is played twice
with the side assignment swapped. There are two disjoint seed sets, both
disjoint from training seeds (`< 700000`, shifted per stage via `--seed-offset`):
- **selection** (`--select-seeds`, base 700000): every `--eval-every`
  iterations. The best score is saved to `--out`.
- **report** (`--report-seeds`, base 800000): kept at `0` throughout training
  and model selection, and measured on the final checkpoint (`--benchmark-only`).

Scenarios and presets rotate over ERYTHEA, ISLADIA, DRAGON, TUTORIA, SORCERY
and HADESHA, × {Beginner-Open, Interm-Diplo, Adv-Hidden, Fog-TotalWar}.

## 6. Results

### Handoff baseline (16-core Mac, 32 games/iter, `d443982`)

| Run | Setup | Selection city share (iterations 10…100) | Best |
|---|---|---|---|
| A | pure self-play | 9, 8, 11, 18, 14, 9 % … (stopped at 68) | 18.2 % @40 |
| B | `--classic-frac 0.3` | 3, 12, 17, 12, 10, 14, 13, 14 % … | 16.5 % @30 |
| C | B + `--local-weight 5 --lambda 0.9` | 9, 15, 14, 10, 12 % … (stopped at 58) | 15.1 % @20 |
| B report | kept checkpoint (iter 30), 48 held-out games | **19.8 % ± 8.4 %** mean share; 305 vs 2,024 cities (13.1 %); Modern largest side in 5/48 | previous `cpp/ai_weights.json` |

### Pure-discovery `1 vs (K − 1)` curriculum on 72-thread Linux (`2026-11-06`)

Trained with **zero hand-coded tactical/economic/diplomatic `if-then-else`
filters** (`--report-seeds 0` throughout curriculum tuning; `pure_s4_1v7` was
checked once on 128 report games before discovering the `midCountdown` and
`HOLD_GARRISON` representation bugs, and the final checkpoint
`pure_s6_1v7_unlocked` was selected strictly on 192 selection games before its
one-time held-out report evaluation; both are reported below for transparency):

| Benchmark Split (Turn 40, all 6 scenarios × 4 presets) | Handoff (`d443982`) | Stage 4 (`pure_s4_1v7`) | **Installed (`pure_s6_1v7_unlocked`)** |
|---|---|---|---|
| **192-game canonical selection** (`--select-seeds 96`, `700000+`) | 16.1 % ± 3.7 % (`975` vs `8,202`; #1 in `18/192`) | 30.1 % ± 4.4 % (`2,227` vs `7,188`; #1 in `33/192`) | **35.3 % ± 4.8 %** (`2,601` vs `6,800`; **#1 in `50/192`**) |
| **48-game held-out report** (`--report-seeds 24`, `800000+`) | 19.8 % ± 8.4 % (`305` vs `2,024`; #1 in `5/48`) | 25.2 % ± 8.7 % (`484` vs `1,854`) | **32.9 % ± 9.4 %** (`628` vs `1,732`; **#1 in `12/48`**) |
| **128-game held-out report** (`--report-seeds 64`, `800000+`) | **18.4 % ± 4.9 %** (`[13.5, 23.3] %`; `743` vs `5,720`; #1 in `13/128`) | 27.7 % ± 5.2 % (`[22.5, 32.9] %`; `1,482` vs `5,006`; #1 in `19/128`) | **32.8 % ± 5.6 %** (`[27.2, 38.4] %`; **`1,710` vs `4,793`**; **#1 in `30/128`**) |
| **256-game extended held-out report** (`--report-seeds 128`, `800000..800127`) | **15.5 % ± 3.2 %** (`[12.3, 18.7] %`; `1,266` vs `11,124`; #1 in `22/256`) | — | **32.1 % ± 4.0 %** (`[28.1, 36.1] %`; **`3,271` vs `9,423`**; **#1 in `55/256`**) |

**Curriculum & ablation highlights:**
- **Small/medium maps:** On cropped `1v1` maps (`K = 2, crop = 14`, 64 games),
  tabula-rasa training (`causal_s1_1v1`, 60 × 288 games, 6 minutes) reached
  **45.7 % ± 11.0 %** mean share and tied Classic AI at **377 vs 377 aggregate
  cities (50.0 %)**. On `2v2` (`K = 4, crop = 14`), `causal_s2_1v3` reached
  **31.5 % ± 8.5 %** (largest side in `19/64`). On smaller shipped scenarios
  (`SORCERY` 20 cities, `DRAGON` 40 cities), Modern AI holds **54–87 %** of
  cities.
- **Why `4×` wider MLPs (`36,526` params, `128→64` / `96→48`) underperformed `8,066` params (`26.9 %` vs `42.0 %` on `1v1`):**
  1. Coordinate-wise Adam without $\mu$P width scaling (`1/width` hidden/output
     LR) produced `4×` larger `W3·h2` logit jumps, collapsing entropy (`0.86 → 0.48`
     vs `0.66`) into premature habits.
  2. `16×` larger `tanh` matrices plus global L2 gradient clipping (`0.5` across
     all `36.5k` params) drowned out the `122`-parameter linear `wSkip` skip
     connection that learns clean fundamentals first in the `8k` model.
  3. `ValueNet` (`3,745` params on 14 active context features) overfit current-batch
     returns (`vloss ≈ 0.02`), adding variance to GAE TD residuals, while
     unbatched scalar C++ backprop over ~30 candidates/decision ran `4×` slower.
- **Diagnostics (`w2aidiag` on held-out `ERYTHEA 800001`, 80 cities, Turn 40):**
  - Instead of the handoff checkpoint's defensive shuttling (`1,004` `Reinforce`
    vs `144` `Siege`), Modern AI executes `1,118` `Siege`, `641` `Intercept`,
    `300` `CaptureNeutral`, and `152` `Bless` moves against `451` `Reinforce`.
  - Without unit-preference rules, `EconNet` buys and produces a combined-arms
    mix (`Archers` 129, `Dwarves` 67, `Heavy Inf.` 53, `Orcish Mob` 30, `Giants`
    23, `Heavy Cav.` 23, `Pegasi` 20, `Griffins` 18, `Light Cav.` 18, `Catapults`
    13, `Minotaurs` 10). On `ERYTHEA 800001`, `side 0 MODERN` leads the entire
    80-city map at Turn 30 (`31` cities, `135` armies, `31` garrisoned in-city)
    and finishes Turn 40 alive with `21` cities and `368` strength.
  - **Remaining gap vs Classic AI on 80-city 8-player maps:** (1) Classic AI
    ignores *Hidden Map* (active in 50 % of benchmark presets) while Modern AI
    explores fog fairly; (2) in Turns 25–40 on 80-city continents, multi-front
    Classic coalitions still erode Modern AI's outer cities (~`95 %` of Modern
    army-turns move rather than garrisoning rear hubs).

**Throughput.** On 72 Xeon threads (`cpp/build-train`, `-DW2_NATIVE=ON`), a
`432`-game full-map iteration (`~12,200` learner turns) takes **20–24 s**; a
`288`-game cropped `1v1` iteration takes **2–8 s**.

## 7. Continuing the work on a large Linux machine

### Setup
```bash
# Ubuntu/Debian: compiler, CMake, Ninja (SDL2 is NOT needed for training)
sudo apt install build-essential cmake ninja-build git
git clone <repo> warlords2-remake && cd warlords2-remake && git checkout ai
# The original game files must be in original/ (same layout as on the dev machine):
ls original/TERRAIN0/ARMYTYPE.DAT
```

### Train (curriculum or `train.sh`)
```bash
# Build headless trainer & run unit tests
cmake -G Ninja -S cpp -B cpp/build-train -DCMAKE_BUILD_TYPE=Release -DW2_BUILD_GAME=OFF -DW2_NATIVE=ON
cmake --build cpp/build-train -j && cpp/build-train/w2test

# Stage 1: 1v1 cropped maps from scratch (--report-seeds 0 throughout tuning!)
mkdir -p runs/s1_1v1
cpp/build-train/w2arena --solo-learner --from-scratch \
  --iterations 60 --games-per-iter 288 --turns 28 \
  --eval-every 10 --eval-turns 28 --select-seeds 32 --report-seeds 0 \
  --active-sides 2 --crop-margin 14 --random-map-frac 0.40 \
  --local-weight 1.5 --lr 3.8e-3 --seed-offset 100000 \
  --out runs/s1_1v1/weights.json --log runs/s1_1v1/log.csv

# Stage 2 -> Stage 3: copy weights forward, widen --active-sides (4 -> 8, crop 0),
# and switch full-map 1v7 polishing to --local-weight 0.0 --temp-start 0.70.
```

### Evaluate, inspect, install
```bash
B=cpp/build-train
$B/w2arena --benchmark-only --select-seeds 96 --report-seeds 0 --eval-turns 40 --out runs/X/weights.json  # 192g selection
$B/w2arena --benchmark-only --report-seeds 64 --eval-turns 40 --out runs/X/weights.json                   # 128g held-out report
$B/w2aidiag runs/X/weights.json ERYTHEA 800001 40          # per-side economy, mission mix, production
cp runs/X/weights.json cpp/ai_weights.json                 # install; commit with the report numbers
```
**Resuming.** Without `--from-scratch`, `w2arena` loads `--out` if it exists.
Pass `--temp-start 0.70 --lr 1.8e-3` when warm-restarting an already competent
checkpoint so high-temperature noise does not wash out learned weights.

### w2arena flags
| Flag | Default | Meaning |
|---|---|---|
| `--iterations N` | 300 | PPO iterations |
| `--games-per-iter G` | 48 | games per iteration (use a multiple of hardware threads, e.g. `288`/`360`/`432`) |
| `--turns T` | 40 | training game cut-off (time limit; critic bootstraps) |
| `--eval-every E` | 10 | selection evaluation interval |
| `--eval-turns T` | 40 | evaluation game cut-off |
| `--select-seeds S` | 12 | selection seeds (×2 games each on full maps) |
| `--report-seeds R` | 32 | held-out report seeds (set `0` during training/tuning!) |
| `--epochs` / `--minibatches` | 4 / 8 | PPO passes and minibatches per iteration |
| `--lr X` | 3e-3 | starting learning rate (decays linearly to 3e-4) |
| `--temp-start T` | 1.0 | starting exploration temperature (anneals to `0.5`) |
| `--seed-offset N` | 0 | offset (`<= 500000`) keeping stage training seeds disjoint from `700000+` |
| `--lambda L` | 0.95 | GAE λ |
| `--local-weight W` | 1 | weight of `G_pool − V_stack(s)` in mission advantages |
| `--solo-learner` | off | `1` recorded Modern learner vs `K − 1` Classic AIs |
| `--active-sides K` | 8 | keep `K` (`2..8`) nearest capitals for curriculum scaling |
| `--crop-margin M` | 0 | crop map to active capitals `± M` tiles (`0` = full map) |
| `--random-map-frac F` | 0 | fraction of training games on generated in-memory random maps |
| `--classic-frac F` | 0 | fraction of non-solo games against the Classic AI |
| `--from-scratch` | off | random init instead of loading `--out` |
| `--out PATH` / `--log CSV` | `cpp/ai_weights.json` / none | checkpoint and log |
| `--benchmark-only` | off | run selection (`--report-seeds 0`) or report (`--report-seeds > 0`) on `--out` |
| `--data DIR` | `original` | game files |

## 8. Recommended next experiments (in order)

1. **Longer-horizon full-map stage (60–80 turns).** On 80-city maps (`ERYTHEA`,
   `ISLADIA`), Modern AI peaks at Turns 20–30 (`31` cities on `ERYTHEA 800001`)
   and cedes border cities in Turns 30–40. Extending the final `1v7` stage to
   `--turns 60 --eval-turns 60` directly exposes late-game consolidation and
   garrison retention to GAE.
2. **Symmetric combat-exchange local credit (without city-loss double counting).**
   Subtracting rival-turn city losses a second time on top of `cur.advantage`
   made 8-player play overly timid (`causal_s3_1v7`), whereas crediting defensive
   enemy kills inside walled cities (`+0.25 · ΔarmyShare` during rivals' turns)
   would reward garrisoning threatened chokepoints without double-counting city
   losses.
3. **$\mu$P-scaled capacity or batched GEMM / spatial encoder.** If scaling width
   beyond `32→16`, scale hidden/output Adam learning rates by `1 / width` and use
   per-head gradient clipping plus batched matrix multiplies so wider networks do
   not collapse entropy or slow CPU backprop `4×`.
4. **Fair-fog Classic benchmark toggle.** Adding an optional evaluation flag
   where Classic AI also obeys *Hidden Map* would separate fog-of-war omniscience
   from pure tactical/strategic strength.
5. **Personalities.** Activate the 10 character-card trait channels in
   `ContextVec` once late-game 80-city consolidation reaches 50 % parity.

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
| `d443982` | `w2aidiag`; the game warns when weights fail to load; run B checkpoint installed (`19.8 % ± 8.4 %` on 48g; `18.4 % ± 4.9 %` on 128g); initial handoff doc. |
| (2026-11-06) | 5th head `StackValueNet` (`8,066` params, v4 JSON with v3 compat); fixed `z_i = u_i/T − ln N` temperature-multiplicity bug, `midCountdown` & `setProduction` economy MDP, multi-unit `HOLD_GARRISON` (`optSv[14]`), decoupled `DisbandArmy`, `uintptr_t` unit lineage, thread-safe in-memory random maps & cropped-map `1 vs K−1` curriculum (`--solo-learner`); installed `pure_s6_1v7_unlocked` (`32.8 % ± 5.6 %` on 128g, `32.1 % ± 4.0 %` on 256g, `35.3 % ± 4.8 %` on 192g selection). |

**Review items deliberately left open** (they do not affect training):
- `Side::classicAi` doubles as the diplomacy rule-variant flag, and it is set on
  human sides too. With Modern AI + the classic UI, a human pays hidden trust
  penalties.
- Run `sh cpp/test/compare.sh` (needs luajit) to re-confirm Classic-AI parity
  with the Lua reference after the shared `diplomacy.cpp` / `game.cpp` changes.

## 10. File map

| File | Role |
|---|---|
| `cpp/src/warlords/ai/modern_ai.cpp` | Turn engine: legal candidates, waves, execution, economy, diplomacy (zero `if-then-else` tactical filters) |
| `cpp/src/warlords/ai/ml_policy.{hpp,cpp}` | 5-head residual MLP (`8,066` params), sampling (`selectFromLogits`, `selectBinary`), context features, empire score, weights I/O |
| `cpp/src/warlords/ai/belief.{hpp,cpp}` | Fair-information belief, fog frontiers, combat estimates, `ScopedTileIndex` |
| `cpp/src/warlords/ai/personality.{hpp,cpp}` | 27 character-card profiles (UI, attitude score; future conditioning) |
| `cpp/src/warlords/diplomacy.{hpp,cpp}` | Alliances, truces, tribute, trust, `computeAttitude` |
| `cpp/src/warlords/scn.{hpp,cpp}`, `game.{hpp,cpp}` | Thread-safe in-memory random maps (`loadFromFiles`) & curriculum map scaling (`activeSides`, `cropMargin`) |
| `cpp/test/arena.cpp` | `w2arena`: PPO self-play & `--solo-learner` curriculum trainer and Classic benchmark |
| `cpp/test/aidiag.cpp` | `w2aidiag`: behaviour diagnostic for a checkpoint |
| `cpp/test/train.sh` | Headless build and training launcher |
| `cpp/test/run.cpp` (`testModernAi`) | Unit tests: fog pathing, diplomacy, sampling, multiplicity prior, `StackValueNet`, cropped maps, engine switching |
| `cpp/ai_weights.json` | Installed `8,066`-parameter checkpoint |

## 11. Playing against it
```bash
cpp/build/warlords2 --window --scenario ERYTHEA --diplomacy --hidden-map               # Modern AI, classic UI
cpp/build/warlords2 --window --modern-ui --scenario ERYTHEA --diplomacy --hidden-map   # with Parley / F10 inspector
cpp/build/warlords2 --window --classic-ai --scenario ERYTHEA                           # the 1993 AI
cpp/build/warlords2 --window --modern-ui --scenario ERYTHEA --spectate                 # watch 8 AIs, full map
cpp/build/warlords2 --weights runs/X/weights.json ...                                  # try a checkpoint
```
