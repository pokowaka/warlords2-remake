# C++ Style & Engineering Guide

How the C++23 codebase (`cpp/`) is designed, written, and verified. Mechanical rules are enforced by `cpp/.clang-format`, `cpp/copts.bzl` (`-Wall -Wextra -Wthread-safety`), and `cpp/.clang-tidy` (`WarningsAsErrors: '*'`); architectural and documentation conventions guide authors and reviewers.

---

## 1. Behavioural Equivalence & Verification Gates

The C++ implementation plays the exact same games as the Lua reference (`love2d/`) turn by turn and generates byte-identical worlds across the C++, Lua, and JavaScript ports. Structural, naming, or architectural changes must never alter a single move, random roll, or rendered pixel. Every commit passes `sh tools/check.sh`:

| Gate | Command | Requirement |
|---|---|---|
| Unit, integration & golden tests | `bazel test //cpp/...` | All 15 Bazel test suites pass (13 GoogleTest suites + `golden_test` + `ui_test`). |
| Lua-vs-C++ game parity | `sh cpp/test/compare.sh` | 15/15 full games match turn by turn. |
| Cross-port worldgen parity | `sh cpp/test/scengen_compare.sh 1 4` | 24/24 generated worlds match byte for byte across C++, JS, and Lua. |
| Static analysis | `sh tools/tidy.sh` | **0 findings** across `cpp/src/` and `cpp/test/`. |

Intentional gameplay, world-generation, or UI visual changes belong in dedicated commits that state the change explicitly and regenerate golden baselines (`bazel run //cpp:golden_update`, `bazel run //cpp:ui_update`). Quirks required for 1993 / Lua parity are preserved and explained in plain English (`// Matches the reference: razed cities count here too.`).

---

## 2. Formatting, Compilation Units & Class Size

- **Formatting:** Format every touched C++ file with `clang-format -i --style=file:cpp/.clang-format` (Google-based, 4-space indent, 100-column limit). Never hand-format against it.
- **Header / Implementation Separation (`.hpp` vs `.cpp`):**
  - `.hpp` files declare class interfaces, member variables, and trivial 1-line inline accessors; prefer forward declarations to keep `#include` graphs shallow.
  - `.cpp` files hold constructors, destructors, non-trivial method bodies, and anonymous-namespace helpers (never mark symbols `static` inside an anonymous namespace).
  - Header-only definitions are restricted to templates (`StrongId<Tag>`, `ResidualMLP<...>`) and `constexpr` expressions.
- **Cohesive Sizing:** Keep classes focused on a single domain responsibility (`< 500` LOC per class). Extract descriptive `using` type aliases at class or namespace scope rather than writing nested generic types (such as `std::function<...>` or `std::vector<std::pair<...>>`) inline in signatures.

---

## 3. Strict OO Encapsulation (`class` vs `struct`)

`cpp/.clang-tidy` enforces `misc-non-private-member-variables-in-classes` (`IgnoreClassesWithAllMemberVariablesBeingPublic: true`):

- **`class` for Invariants & Behaviour:** Entities (`Army`, `City`, `Side`, `Map`, `Game`, `ArmyRegistry`, `DiplomacyMatrix`, `FogOfWar`, `TurnSchedule`, `Quest`), domain services (`Pathfinder`, `CombatResolver`, `TurnEngine`, `CityEconomy`, `QuestSystem`, `SiteExplorer`, `HeroSystem`), strategies (`IComputerPlayer` subclasses), RAII wrappers, and UI controllers/modals (`kit::Modal`, `UiContext`, `MapScreenController`, `IMapScreenState`) are `class`es whose data members are **100% `private:`** with trailing underscores (`lower_case_`). Mutations flow through methods that preserve class invariants (such as spatial-index synchronization on `Map` and `ArmyRegistry`, or `Pathfinder` invalidation on `Game::SetCityOwner`).
- **`struct` for Passive Value Records:** Plain data aggregates with zero hidden invariants (`TilePos`, `TurnReport`, `AttitudeBreakdown`, `WalkSpec`, `ApplicationConfig`) are `struct`s with **100% `public`** `lower_case` fields (no trailing underscore).

---

## 4. Naming & Identifier Length

Checked mechanically by `readability-identifier-naming` and `readability-identifier-length`:

| Construct | Convention | Example |
|---|---|---|
| Types, `enum class`es, `using` aliases | `CamelCase` | `CombatResolver`, `SideId`, `ArmyStack` |
| Functions and methods | `CamelCase` (verb for actions) | `ResolveAttack()`, `MarkExplored()` |
| Local variables and parameters | `lower_case` | `defending_side`, `tile_x` |
| `class` data members (`private:`) | `lower_case_` | `armies_`, `turn_mutex_` |
| `struct` fields (`public:`) | `lower_case` | `report.diplo_news`, `pos.x` |
| Constants, `constexpr`, enumerators | `kCamelCase` | `kMaxStack`, `WalkStop::kOutOfMoves` |
| Namespaces | `lower_case` | `w2::ai::ml` |
| Macros (avoid outside `ABSL_FLAG` / `ABSL_CHECK`) | `UPPER_CASE` | |

- **Descriptive Names (`>= 3` characters):** Only `i`/`j` (loop counters), `x`/`y`/`dx`/`dy` (coordinates), and `n` (count parameter) may be shorter than three letters. Avoid single-letter or cryptic abbreviations (`game` not `g`, `side` not `s`, `city` not `c`, `screen_x` not `sx`).
- Standard container iteration methods (`size`, `begin`, `end`, `empty`, `data`) retain STL lowercase spelling where range-`for` compatibility is intended.

---

## 5. C++23, Abseil & JSON Vocabulary

- **Strong IDs & Sum Types:** Use `SideId`, `CityId`, `ArmyId`, `ArmyTypeId`, `SiteId`, `ItemId` (`warlords/model/ids.hpp`) instead of raw integer entity indices, `enum class` instead of stringly-typed mode tags, and `std::variant` (`VectorTarget`, `Quest::Target`, `TurnEvent`) for closed sum types. Every `switch` over an `enum class` enumerates all values without a `default` branch so `-Wswitch` flags unhandled cases at compile time.
- **Filesystem & Binary Buffers:** Use `std::filesystem::path` for file and directory paths (never raw `std::string` path concatenation) and `std::vector<uint8_t>` / `absl::Span<const uint8_t>` for binary payloads (never `std::string` or `char*` as byte buffers).
- **Abseil Containers, Views & Time:** Prefer `absl::flat_hash_map` / `absl::flat_hash_set` for hash tables, `absl::Span<const T>` and `std::string_view` for non-owning read-only views, `absl::StrContains` / `absl::StartsWith` / `absl::StrFormat` (or `<format>` / `<charconv>`), and `absl::Time` / `absl::Duration` (never `std::chrono` in application/library code).
- **JSON (`nlohmann::ordered_json`):** Compiled with `-DJSON_NOEXCEPTION=1` (`cpp/copts.bzl`). Always parse with `nlohmann::ordered_json::parse(input, nullptr, /*allow_exceptions=*/false)` and check `parsed.is_discarded()`.
- **CLI Flags (`absl::flags`):** Define CLI flags with `ABSL_FLAG(...)` and parse via `w2::ParseCliFlags(argc, argv, usage)` (`util/util.hpp`), which normalizes `--kebab-case` flags for shell-script compatibility.
- **Casts & Literals:** Use `static_cast` / `reinterpret_cast` (never C-style casts), uppercase literal suffixes (`1.0F`, `16UZ`), and widen before multiplying (`static_cast<size_t>(width) * height`). Mark single-argument constructors `explicit`.

---

## 6. Exception-Free Error Handling (`absl::StatusOr<T>`)

`cpp/src/` is **100% exception-free** (zero `throw`, `try`, or `catch`):

- **`absl::Status` / `absl::StatusOr<T>` vs `ABSL_DCHECK` (Abseil TotW #181):**
  - Return `absl::Status` or `absl::StatusOr<T>` for operational, I/O, asset-decoding, and external-input failures (`scn::Load`, `pck::Load`, `save::Decode`, `PreferencesStore::Save`, `PolicyNetwork::LoadJson`).
  - Use `ABSL_DCHECK` / `ABSL_CHECK` (`absl/log/absl_check.h`) for internal programmer preconditions and invariant checks.
- **Actionable 4-Part Error Messages:** Every constructed `absl::Status` error message states:
  1. **Operation:** what failed (`Failed to load scenario`),
  2. **Target:** the file path, identifier, or slot (`'original/ERYTHEA/ERYTHEA.SCN'`),
  3. **Detail:** the concrete reason, byte offset, or `errno` (`truncated header at byte 12`), and
  4. **Remediation hint:** how the caller or user can fix it (`verify --data points to a valid Warlords II directory`).
- **Errors as Data & Naming:** Propagate `absl::Status` values up to CLI or UI boundaries rather than logging at the failure site and also returning an error. Name `absl::StatusOr<T>` variables after the domain value (`auto map = scn::Load(...)`), never with `_or` / `_status_or` suffixes.

---

## 7. Zero Mutable Globals & Concurrency (`-Wthread-safety`)

- **Zero Non-Const Globals:** `cppcoreguidelines-avoid-non-const-global-variables` is enforced across `cpp/src/` and `cpp/test/`. Pass state explicitly via `Game&`, `UiContext&`, `PreferencesStore&`, or `IScenarioSource&`. Where process-lifetime caches or registries are genuinely required, encapsulate them in a thread-safe class instantiated inside an accessor via `absl::NoDestructor<T>`.
- **Dijkstra Monitors & Clang Thread Safety:**
  - Protect shared mutable state with domain-named `absl::Mutex` members (`turn_mutex_`, `registry_mutex_`, `deploy_temp_mutex_`) — never generic `mu_` or `mutex_`.
  - Annotate all guarded members with `ABSL_GUARDED_BY(...)` and methods with `ABSL_EXCLUSIVE_LOCKS_REQUIRED(...)` or `ABSL_LOCKS_EXCLUDED(...)`, verified statically by `-Wthread-safety`.
  - Wait on state transitions with `absl::Condition` member predicates (`turn_mutex_.Await(absl::Condition(this, &AiTurnRunner::IsMainTurn))`) rather than polling or sleeping.
  - **No Alien Code Under Lock:** Snapshot callbacks, observer events, or audio buffers under `absl::MutexLock`, release the lock, and invoke external/virtual code outside the critical section.

---

## 8. Consumer-Focused Doxygen Documentation

- Document every non-trivial file (`@file` + `@brief`), class, struct, enum, and public function with `/** ... */` Doxygen blocks written for a first-time API consumer:
  1. **Architectural role** (what domain concept or boundary the type represents),
  2. **Data flow & units** (for pipelines, renderers, or coordinate spaces),
  3. **Invariants** (state equations that hold outside critical sections / mutators),
  4. **Thread safety & ownership/lifetime contracts**, and
  5. **`@param` / `@return`** when constraints or lifecycles are not obvious from the signature, plus trailing `///<` on members and enumerators.
- **Prohibited in Comments:**
  - No PR or refactoring-history narration (never write *"Decoupled from X"* or *"Replaces legacy Y"*).
  - No redundant `@class Foo` / `@fn Bar` tags or numbered inline step lists (`// 1. ...`).
  - No 1993 executable segment:offset addresses (`6d51:1244`) in C++ comments (reverse-engineering notes belong in `docs/re/` and `docs/formats/`).

---

## 9. Testing & Developer Tooling

- **Hermetic GoogleTest Suites:** Add unit tests to the corresponding per-layer `@googletest` `cc_test` target in `cpp/BUILD.bazel` (`model_gtest`, `formats_and_ui_data_test`, `rules_*_test`, `ai_*_test`, `worldgen_test`, `persistence_test`, `platform_raii_test`, `ui_core_test`), using `TEST_P` for multi-scenario coverage. Tests assert observable public contracts, never call `sleep()`, and run SDL2 targets headlessly via `SDL_VIDEODRIVER=dummy` and `W2_USER_DIR`.

| Task | Command |
|---|---|
| Run all verification gates | `sh tools/check.sh` |
| Build everything | `bazel build //cpp/...` |
| Run all tests (uncached) | `bazel test --nocache_test_results //cpp/...` |
| Run Address + Undefined Behaviour Sanitizers | `bazel test --config=asan //cpp/...` |
| Launch the game | `bazel run //cpp:play [-- --scenario ERYTHEA --window]` |
| Regenerate `compile_commands.json` | `python3 tools/compile_commands.py` |
| Run `clang-tidy` (`0 findings` required) | `sh tools/tidy.sh [path-regex]` |
| Package distributable archive | `sh tools/package.sh` |
