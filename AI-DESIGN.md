# Modern AI: design, training, and how to continue (`AI-DESIGN.md`)

This document is the handoff for the learnable "Modern AI" on the `ai` branch:
what it is, how it is trained and measured, what has been learned so far, and
exactly how to continue training on a larger machine.

> [!IMPORTANT]
> **Status (2026-11-07).** The installed checkpoint (`cpp/ai_weights.json`, v6,
> 9,700 parameters) comes from an overnight re-run of the pure-discovery
> curriculum from scratch with v6 inputs and the privileged critic. Played
> near-greedily (`DEPLOY_TEMPERATURE` = 0.02), it scores **44.3 % city share on
> 256 held-out report games** against the Warlord Classic AI (50 % = parity),
> up from **32.1 %** for yesterday's checkpoint at T = 0.5 and 40.9 % for it at
> T = 0.02. The remaining weakness is total war and the late game: it leaves
> captured cities empty. Recipe tweaks, Raider/frozen opponents, departure
> credit, lookahead search and imitation of the Classic AI were tried and did
> not help (§6.3, each with numbers).

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

**Sampling in real games.** Real games sample the policy at
`DEPLOY_TEMPERATURE` = 0.02 (`ml::deployTemperature()`), i.e. near-greedy. The
generator is seeded from the game's dice state, the turn and the side, and
never advances the game's RNG, so games stay reproducible. Training still
anneals to `TRAIN_TEMPERATURE_END` = 0.5, because PPO needs exploration. A
sweep with the installed weights (512 dev games each, turn 40) was monotonic:
T = 0.5 → 35.9 %, 0.25 → 38.2 %, 0.15 → 39.7 %, 0.08 → 42.4 %, 0.02 → 43.4 %
city share; on the held-out report set 32.1 % → 40.9 % (§6). (An earlier
observation that greedy play was worse was made with the much weaker handoff
checkpoint.)

### The network (`ml_policy.hpp`)
Five residual MLPs, `y = W_skip·x + W3·tanh(W2·tanh(W1·x + b1) + b2) + b3`,
**9,700 parameters** in total, with hand-written forward and backward passes
(no ML library):

| Head | Input | Output |
|---|---|---|
| DiploNet 48→24→12→6 | context 30 + rival 18 | 4 stance logits, tribute logit, joint-war logit |
| MissionNet 67→32→16→2 | context 30 + stack 16 + target 21 | utility, stage-near logit |
| EconNet 51→24→12→1 | context 30 + econ 21 | score (disjoint `[0..6]` selects decision type; `[20]` flags current choice) |
| ValueNet 46→24→12→1 | context 30 + critic 16 | V(s), empire-level critic in empire-score units |
| StackValueNet 62→24→12→1 | context 30 + stack 16 + critic 16 | V_stack(s), pre-decision pool critic for multi-turn per-stack local returns |

**Privileged critics (asymmetric actor-critic).** The two critics are only used
to compute training advantages, so they also read 16 true-state features
(`buildCriticFeatures`: true city/income/army shares, strongest rival, neutrals,
map size, sides alive/in use, Hidden Map and Diplomacy flags, share of Classic
rivals, threatened own cities, empire score, an unsaturated turn counter). The
policy heads never see them. This raised the critic's explained variance from
~0.56 to ~0.70 (logged as `ev` per iteration).

Context is 20 global fair-information features plus 10 personality channels,
which are held at 0 for now. Features 14–19 (added in v6) are gold, turn and own
army strength on non-saturating scales, the share of own cities that are empty,
the share empty *and* reachable within a turn by a visible enemy, and the budget
balance (income − upkeep) / income.

**Multiplicity prior.** Option families have very different sizes (40 fog
frontiers vs 1 hold). The policy logit at temperature `T` is therefore
`z_i = u_i / T − ln N_family(i)`, sampled via `softmax(z)`. Subtracting
`ln N_family(i)` *outside* `1/T` ensures every equal-utility family receives equal
probability mass at any temperature (fixing an earlier `(u_i − ln N)/T` bug that
penalised multi-option families by `1/N²` at `T = 0.5`). A KL pull toward the
uniform-family prior `q_i ∝ 1 / N_family(i)` is the entropy regulariser.

Weights live in `cpp/ai_weights.json` (format version 6, `9,700` parameters).
Version 5 (`8,866`), 4 (`8,066`) and 3 (`6,729`) files load function-preserving
(`unflattenLegacy`): input columns an older format lacked start at zero (and v3's
missing `StackValueNet` is zero-initialised), so a migrated network plays exactly
as before until training uses the new inputs.

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

### 6.1 Handoff baseline (16-core Mac, 32 games/iter, `d443982`)

| Run | Setup | Selection city share (iterations 10…100) | Best |
|---|---|---|---|
| A | pure self-play | 9, 8, 11, 18, 14, 9 % … (stopped at 68) | 18.2 % @40 |
| B | `--classic-frac 0.3` | 3, 12, 17, 12, 10, 14, 13, 14 % … | 16.5 % @30 |
| C | B + `--local-weight 5 --lambda 0.9` | 9, 15, 14, 10, 12 % … (stopped at 58) | 15.1 % @20 |
| B report | kept checkpoint (iter 30), 48 held-out games | **19.8 % ± 8.4 %** mean share; 305 vs 2,024 cities (13.1 %); Modern largest side in 5/48 | previous `cpp/ai_weights.json` |

### 6.2 Pure-discovery `1 vs (K − 1)` curriculum on 72-thread Linux (`2026-11-06`)

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

### 6.3 Discoveries of 2026-11-06 (afternoon session)

All numbers are Modern city share in mixed 4 vs 4 games against the Warlord
Classic AI at turn 40 (50 % = parity), on the **development seed set** (750000+,
`--dev-seeds`, never used for selection or the report) unless marked *report*.
"Paired" means both checkpoints played the same seeds (`--compare`); with 512
games its 95 % interval is about ± 2.8 %, so smaller effects are not resolvable.

**Measurement.**
- The benchmark is deterministic: re-running a checkpoint reproduces it exactly.
- The selection eval used to keep the best of ~10 noisy (± 6.5 %) measurements,
  which inflates the kept score (winner's curse). Compare finals or snapshots
  with `--compare` on dev seeds instead.
- Diplomacy, not Hidden Map, splits the presets: installed weights at T = 0.5
  scored Beginner-Open (no fog, no diplomacy) 27.7 %, Interm-Diplo 55.2 %,
  Adv-Hidden 31.9 %, Fog-TotalWar 28.9 %. Total war is the weak spot.
- Solo 1 vs 7 (the training setup) on 8-player maps: 3–5 % vs 12.5 % parity.

**Deployment temperature — the one big win.** Same weights, 512 dev games each:

| T | 0.5 | 0.25 | 0.15 | 0.08 | 0.02 |
|---|---|---|---|---|---|
| city share | 35.9 % | 38.2 % | 39.7 % | 42.4 % | **43.4 %** |

Held-out *report* (256 games): 32.1 % at T = 0.5 → **40.9 %** at T = 0.02. Every
preset improves (Interm-Diplo reaches 61.6 %). Training still samples at 0.7 → 0.5.

**Critic.** Privileged true-state critic inputs raised explained variance from
~0.56 to ~0.70, but did not by themselves make the policy measurably stronger.

**Recipe variants, all warm-started from the installed weights** (48–64
iterations × 432 games, paired vs installed at T = 0.02):

| Run | Change | Paired B − A |
|---|---|---|
| E1 | privileged critic, s6 recipe | +1.3 % ± 2.9 % |
| E2 | learning rate 5e-3 (2.8×) | −0.7 % ± 2.9 % |
| E3 | 50 % of games 4 vs 4, all learner seats recorded | +1.0 % ± 2.7 % |
| E4 | training temperature 0.25 → 0.15 | −1.3 % ± 2.5 % |
| E5 | 60-turn games (judged at turn 60) | −0.8 % ± 3.0 % |
| E6 | 50 % of Classic seats are Raiders, 64 iterations | −0.8 % ± 2.9 % |
| E9 | departure credit (weight 2) + all games 4 vs 4 (shared live policy) | **−3.2 % ± 2.7 %** (significantly worse) |
| E10 | 1 vs 7, half the opponents the frozen installed AI, departure credit | −2.0 % ± 2.9 % |
| E11 | search as teacher (K=4, R=2, H=2, peeking teacher) | **−4.6 % ± 3.0 %** final; −1.0 % best |
| soups | weight average of E1–E3 checkpoints | −0.6 % / +1.0 % |

KL per iteration stays ~0.001–0.002 whatever the learning rate: the recipe sits
on a plateau rather than being step-size-limited.

**Hand-rule probes (diagnostics only, never part of the Modern AI)** all made it
*weaker*, which says the learned behaviour is a coherent strategy, not a set of
separable bugs: "always build + buy the strongest slot when ≥ 1,500 gold"
(31.3 % vs 35.9 % at T = 0.5; 39.0 % vs 43.4 % greedy), "≥ 1 / ≥ 2 garrison per
city" (32.6 % / 30.9 % vs 35.9 %), "leave the weakest unit behind" (39.9 % vs
43.4 % greedy). The economy *looks* broken (1–2 slot purchases per 40-turn game,
a third of city-turns idle, up to 18k gold banked) but is not the binding
constraint.

**Why it fades: empty cities.** Strength by game length (installed, greedy):
turn 20 **51.3 %**, turn 40 ~43 %, turn 60 40.9 %, turn 80 39.3 %
(Fog-TotalWar 41.5 → 26.6 → 23.8 %). `w2aidiag` capture/loss tracking over 12
total-war games: the Modern camp loses **~300 cities per game, ~88 % with zero
defenders**, and recaptures a similar number (much of it Modern-vs-Modern
ping-pong over empty cities); the Classic camp loses ~90, ~70 % of them defended
by 2+ armies. The policy already *sees* that a move empties its city (stack input
`s[14]`), but the loss arrives during a rival's turn and is shared by the ~70
moves of that turn, so the move that caused it learns almost nothing.

**Opponents instead of rules.** The Modern AI must discover its own strategy, so
pressure comes from training opponents (`cpp/test/opponents.cpp`). The
**Raider** plays the Classic AI but first walks its weakest armies into every
reachable undefended enemy city, the learner's first. Raiders take 30–40 more
cities per game than plain Classic sides; the installed AI scores 41.1 % against
them. Training with 50 % Raider seats alone (E6) did not help (table above):
the punishment is still a turn-level signal shared by every move.

**Departure credit and empty-city inputs** (`7c7d995`):
- `--departure-weight W`: a move that leaves an own city empty (still empty at
  turn end) is charged that city's weight in the empire score if the city falls
  before our next turn, centred over all city-emptying moves (so it learns *when*
  to leave, not "never leave"). It only re-attributes a loss already in the
  reward. The trainer logs "emptied X % of moves, Y % fell".
- Six fair context inputs (v6): non-saturating gold, turn and army strength,
  share of own cities empty, share empty *and* reachable by a visible enemy,
  budget balance.

What happened:
- In 1 vs 7 training only ~10 % of emptied cities fall before the next turn
  (E7, stopped early): too little signal. In 4 vs 4 games it is ~18 %, because
  Modern sides walk into each other's empty cities.
- E9 (departure credit in 4 vs 4 games) *did* move the training metric: moves
  that empty a city 15.1 % → 13.8 %, emptied cities that fall 18 % → 14 %. Yet it
  was significantly worse against Classic (−3.2 % greedy, −2.5 % at T = 0.5) and,
  under greedy play, lost **more** empty cities (≈ 441 vs 388 per game).
- **Lesson — co-adaptation in shared-policy self-play.** All Modern seats in E9's
  games were the same live policy, so "fewer emptied cities fall" can be the
  *attacking* copies taking fewer of each other's cities as much as the
  defenders defending. A metric measured inside self-play can improve without
  the behaviour transferring. Opponents that exploit a weakness must not learn
  alongside the learner: use frozen opponents.

**Frozen opponents (E10).** 1 vs 7 with half the opponent seats played by the
*frozen* installed Modern AI at deployment temperature, plus departure credit:
emptied cities that fall 22 % → 18 %; −2.0 % ± 2.9 % vs Classic (paired), but
**53.1 % ± 3.6 %** vs the frozen installed AI (installed vs itself = 50.0 %).
Strength is opponent-dependent: training against one opponent improves play
against that opponent first.

**Lookahead search — the second big win** (`w2search`, `cpp/test/search.cpp`).
At a Modern turn, build K candidate turns sampled from the policy (plus the
greedy one), copy each R times with different dice (`save::encode/decode`),
play H rounds on with everyone's normal AI, and keep the candidate with the best
average empire score. Same installed weights, 128 dev games, paired:

| Preset | no search | search (K=4, R=2, H=2) | paired difference |
|---|---|---|---|
| all | 44.3 % | **55.5 %** | **+11.2 % ± 5.6 %** (better in 71, worse in 28) |
| Beginner-Open (no fog) | 38.7 % | 47.5 % | +8.9 % |
| Interm-Diplo | 62.7 % | 69.7 % | +7.1 % |
| Adv-Hidden | 40.8 % | 52.2 % | +11.4 % |
| Fog-TotalWar | 35.0 % | 52.4 % | +17.5 % |

> [!WARNING]
> **This +11.2 % is not real planning — it is peeking.** In this first version
> each candidate turn was played once on a copy *with the real game's dice*, and
> that very result was kept, so the search saw its own battles' outcomes before
> committing. The honest version (`w2search … honest`: a candidate is a policy
> sampling seed, every rollout replays the turn with fresh dice, and the chosen
> one is then played with the real dice) is **worse than no search: −6.0 % ± 5.7 %**
> on the same 128 games (Beginner-Open −12 %, Interm-Diplo −15 %, Adv-Hidden 0 %,
> Fog-TotalWar +3 %). Two reasons: alternatives sampled at T = 0.5 are much weaker
> than the greedy turn, and with 2 rollouts × 2 rounds the "best" candidate is
> mostly the luckiest one (winner's curse). Any search must re-roll everything it
> cannot know, and only switch away from the greedy turn when an alternative is
> clearly better than the evaluation noise.

Further caveats of the peek version: (1) the copies simulate the *true* game,
so it also sees through fog and through stacks it has not fought. (2) Playing through a copy changes play slightly
(no-search through the copy path: 45.7 % vs 45.2 % direct on 64 games), so the
baseline is measured through the same path. (3) Cost ≈ 25 min of 72 threads per
128 games (~1–2 s per searched turn).

**Search as a teacher** (`--search-k K --search-r R --search-h H`): every
learner turn is sampled K times from the same position and recorded; each
candidate's advantage is its rollout value minus the mean of the K candidates;
the game continues from the best one. This replaces the turn-level GAE signal
(one noisy score shared by ~70 moves and many turns) with a comparison of
alternatives from the same position. The privileged copies are used only to
train; the trained policy plays on fair information. First update: KL 0.007,
3–4× the per-iteration movement of PPO. **E11** (24 iterations × 36 games,
K=4, R=2, H=2, its teacher still peeking at each candidate's own dice) was
judged without search: final −4.6 % ± 3.0 % (significantly worse), best
selection checkpoint −1.0 % ± 2.9 %. The fair search scored the same as the
honest one (38.3 % vs 44.3 % without search on 128 games).

**Proper fair search** (`w2search … fair 0.1 2.0`: 3 alternatives drawn at
T = 0.1, 6 rollouts × 2 rounds each, fresh dice, unseen enemy field armies
removed, switch away from the greedy turn only when an alternative is > 2
standard errors better): **+2.3 % ± 3.3 %** paired on 128 dev games (46.6 % vs
44.3 %; better in 53, worse in 39), ~1 h of 72 threads per 128 games. Right
direction, not significant. **Confirmation on 256 new dev games: −0.1 % ± 2.1 %**;
pooled over 382 games +0.7 % ± 1.8 %. Honest, fair lookahead at this budget
does not make the AI stronger; it would need a far larger budget or a better
evaluation than the score after 2 rounds.

**Imitating the Classic AI** (`w2imitate`, `modern::snapshotMissions`): all-
Classic games; before a recorded side's turn the Modern AI lists the options it
would weigh for each army group (dry run), and after the Classic turn each
group's decision is labelled with what its moving part did (hold when nobody
left). On those decisions **the Classic AI holds its city in ~60 % of cases, the
Modern AI's greedy choice in 0.4 %**. Behaviour cloning on MissionNet reaches
67 % agreement (from 3.7 %) but plays far worse: **11.0 % vs 45.1 %** (256 dev
games); weight blends with the installed network get monotonically worse
(10 %: −1.7 %, 25 %: −2.9 %, 50 %: −14.0 %). The Classic AI's rare attacks come
from multi-turn plans (gather, then strike, with remembered orders) and from
seeing through fog; a per-decision imitator on fair information learns the
majority ("hold") and stops expanding.

**Overnight curriculum from scratch — the new installed checkpoint.** The
pure-discovery curriculum re-run from random weights with v6 inputs and the
privileged critic (stages 1v1 cropped 100 it → 1v3 80 → 1v5 48 → 1v7 full maps
80 → polish 96; ~3 h on 72 threads; reproducible with `cpp/test/curriculum.sh`). Selection (greedy, 64 games): 1v7 stage peaked at
50.5 % (iteration 60). Paired vs the previous installed weights:

| Checkpoint | dev, turn 40 (512 games) | report, turn 40 (256 games) | dev, turn 60 (256 games) |
|---|---|---|---|
| 1v7 stage, iteration 60 (installed) | **+3.5 % ± 3.2 %** | +3.5 % ± 4.2 % (44.3 % vs 40.9 %; better 134, worse 81) | +3.4 % ± 5.0 % |
| 1v7 stage, final | +1.0 % ± 3.0 % | — | — |
| polish stage, best / final | +0.9 % / −1.5 % | — | — |

The report check is independent of every choice made; combined with the dev
result the gain is about +3.5 % ± 2.5 %. Per preset on the report set:
Adv-Hidden 35.1 → 45.2 %, Interm-Diplo 58.3 → 63.5 %, Fog-TotalWar 32.8 →
35.4 %, Beginner-Open 37.3 → 33.2 %. The polish stage (T 0.65 → 0.5) did not
improve on the 1v7 stage.

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

The whole curriculum, as run for the installed weights, is one script:
`cpp/test/curriculum.sh [OUT_DIR]` (all five stages, then paired compares of
the candidates against `cpp/ai_weights.json`; ~3 h on 72 threads).

### Evaluate, inspect, install
```bash
B=cpp/build-train
$B/w2arena --benchmark-only --select-seeds 96 --report-seeds 0 --eval-turns 40 --out runs/X/weights.json  # 192g selection
$B/w2arena --benchmark-only --report-seeds 64 --eval-turns 40 --out runs/X/weights.json                   # 128g held-out report
$B/w2arena --out cpp/ai_weights.json --compare runs/X/weights.json --dev-seeds 256 --breakdown  # paired A/B on dev seeds
$B/w2aidiag runs/X/weights.json ERYTHEA 750011 40 0 8 0 -1 0 raider  # economy, captures/losses by garrison, missions
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
| `--temp-start T` / `--temp-end T` | 1.0 / 0.5 | training temperature, annealed linearly |
| `--eval-temp T` | 0.02 | temperature evaluation games are played at (`DEPLOY_TEMPERATURE`) |
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
| `--compare PATH` | — | play `--out` (A) and PATH (B) on the same seeds; paired difference with 95 % interval |
| `--dev-seeds N` | 0 | benchmark/compare on the development set (750000+) |
| `--eval-mode mixed\|solo` | mixed | 4 vs 4 alternating seats, or 1 Modern vs K − 1 Classic |
| `--eval-opponent classic\|raider` | classic | which opponent evaluation games use |
| `--breakdown` | off | per-preset and per-scenario shares |
| `--critic-warmup N` | 0 | first N iterations train only the critics |
| `--mixed-frac F` | 0 | in `--solo-learner` runs, share of games played 4 vs 4 with all learner seats recorded |
| `--raider-frac F` | 0 | share of Classic training seats played by the Raider |
| `--departure-weight W` | 0 | departure credit (§6.3) |
| `--save-every N` | 0 | also write `<out>.iterN.json`; the last iteration is always `<out>.final.json` |
| `--frozen-opponent PATH` / `--frozen-frac F` | — / 0 | share of non-learner training seats played by a fixed Modern checkpoint |
| `--search-k K` / `--search-r R` / `--search-h H` | 0 / 2 / 2 | search-as-teacher training (K candidate turns, R rollouts, H rounds) |

## 8. Recommended next experiments (in order)

1. **Search as a teacher** (in progress, E11): judge the trained network
   *without* search against the installed one; if it helps, iterate (expert
   iteration) and scale K/R/H. In parallel: a *fair* play-time search that
   samples the unseen (fog, unfought stacks) instead of copying the true game.
2. **More opponent personalities** (league of exploiters): *Rusher* (all Classic
   sides target the learner's capital early), *Turtle* (heavy garrisons, local
   counter-attacks only), *Hero hunter*. Mix them with plain Classic seats.
3. **A learned exploiter**: a second Modern network trained only to beat the
   current learner, alternating with training the learner against it.
4. **Bigger paired evaluations** (2,000+ games) to resolve +1–2 % effects.
5. **Fair-fog Classic benchmark toggle** and **personalities** remain open.

Tried and not helpful on their own (§6.3): higher learning rate, 4v4 training
games, low training temperature, 60-turn training games, weight averaging.

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

| `e35d923` | Privileged critic inputs (v5, 8,866 params); `--compare`, `--dev-seeds`, `--eval-mode`, `--breakdown`, `--critic-warmup`, `--mixed-frac`, `--save-every`, explained-variance logging. |
| `391f577` | `w2aidiag` economy statistics. |
| `b1a0da3` | Real games near-greedy (`DEPLOY_TEMPERATURE` 0.02; training anneals to `TRAIN_TEMPERATURE_END` 0.5); `--eval-temp`, `--temp-end`. Report 32.1 % → 40.9 %. |
| `efcd30a` | Raider training opponent (`cpp/test/opponents.cpp`), `--raider-frac`, `--eval-opponent`; `w2aidiag` captures/losses by garrison. |
| `7c7d995` | Departure credit (`--departure-weight`) and six empty-city/scale context inputs (v6, 9,700 params; v5/v4/v3 load function-preserving). |

| `56be37b` | Frozen Modern training opponents (`--frozen-opponent`, `--frozen-frac`, `--eval-opponent frozen`). |
| `6870c09` | `w2search` lookahead experiment (first version peeked at its own dice: +11.2 %; honest version −6.0 %). |
| `96038bc` | Search-as-teacher training mode (`--search-k/-r/-h`). |
| `de8afd3`, `fc868d6` | `w2search` honest / fair judges, candidate temperature and conservative switching. |
| `09d3aa8` | Classic imitation experiment (`w2imitate`, `modern::snapshotMissions`). |

| (2026-11-07) | Installed the overnight from-scratch curriculum checkpoint (report 40.9 % → 44.3 % at T = 0.02). |

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
| `cpp/src/warlords/ai/ml_policy.{hpp,cpp}` | 5-head residual MLP (`9,700` params), privileged critic features (`buildCriticFeatures`), sampling (`selectFromLogits`, `selectBinary`), context features, empire score, weights I/O |
| `cpp/src/warlords/ai/belief.{hpp,cpp}` | Fair-information belief, fog frontiers, combat estimates, `ScopedTileIndex` |
| `cpp/src/warlords/ai/personality.{hpp,cpp}` | 27 character-card profiles (UI, attitude score; future conditioning) |
| `cpp/src/warlords/diplomacy.{hpp,cpp}` | Alliances, truces, tribute, trust, `computeAttitude` |
| `cpp/src/warlords/scn.{hpp,cpp}`, `game.{hpp,cpp}` | Thread-safe in-memory random maps (`loadFromFiles`) & curriculum map scaling (`activeSides`, `cropMargin`) |
| `cpp/test/arena.cpp` | `w2arena`: PPO self-play & `--solo-learner` curriculum trainer and Classic benchmark |
| `cpp/test/aidiag.cpp` | `w2aidiag`: behaviour diagnostic for a checkpoint (economy, captures/losses by garrison, missions) |
| `cpp/test/opponents.{hpp,cpp}` | Training-only opponents: Raider (on the Classic AI) and Frozen (a fixed Modern checkpoint) |
| `cpp/test/search.cpp` | `w2search`: lookahead over candidate turns (probe / play) |
| `cpp/test/train.sh` | Headless build and training launcher |
| `cpp/test/curriculum.sh` | The from-scratch curriculum that produced the installed weights |
| `cpp/test/run.cpp` (`testModernAi`) | Unit tests: fog pathing, diplomacy, sampling, multiplicity prior, `StackValueNet`, cropped maps, engine switching |
| `cpp/ai_weights.json` | Installed checkpoint (v6, `9,700` parameters; overnight curriculum, 1v7 stage iteration 60) |

## 11. Playing against it
```bash
cpp/build/warlords2 --window --scenario ERYTHEA --diplomacy --hidden-map               # Modern AI, classic UI
cpp/build/warlords2 --window --modern-ui --scenario ERYTHEA --diplomacy --hidden-map   # with Parley / F10 inspector
cpp/build/warlords2 --window --classic-ai --scenario ERYTHEA                           # the 1993 AI
cpp/build/warlords2 --window --modern-ui --scenario ERYTHEA --spectate                 # watch 8 AIs, full map
cpp/build/warlords2 --weights runs/X/weights.json ...                                  # try a checkpoint
```
The Military Advisor (`--military-advisor`, active by default; `--no-military-advisor` to disable) speculates on combat odds via Shift-hover/click over adjacent enemy armies and cities.
