# C++ style guide

How the C++ port (`cpp/`) is written. The tools check what they can
(`cpp/.clang-format`, `cpp/.clang-tidy`); the rest is for the reader and the
reviewer. When this guide and old code disagree, the guide wins and the code
gets fixed as it is touched.

## The rule above all: behaviour stays the same

The C++ port plays exactly the games the Lua remake (`love2d/`, the
reference) plays, turn by turn, and makes exactly the same worlds as the
JavaScript and Lua ports. A change that restructures or renames code must not
change a single move, roll or pixel. Every commit passes:

| Check | Command |
|---|---|
| Rules suite, golden games and worlds, golden screens | `bazel test //cpp/...` |
| The same games as the Lua remake | `sh cpp/test/compare.sh` |
| The same worlds in all three ports | `sh cpp/test/scengen_compare.sh 1 4` |
| No new clang-tidy findings | `sh tools/tidy.sh` |

`sh tools/check.sh` runs them all. A change that is *meant* to change the
games, the worlds or the screens is its own commit, says so, and records the
golden files anew (`bazel run //cpp:golden_update`, `//cpp:ui_update`).

Things that look odd are often what the reference does. Before "fixing"
anything that changes behaviour, check the Lua; keep it, and say why in a
comment.

## Formatting

`clang-format -i` with `cpp/.clang-format` (Google based, 4-space indent,
100 columns). Never format by hand against it.

## Names

Google style, checked by clang-tidy (`readability-identifier-naming`):

| What | Style | Example |
|---|---|---|
| types, enums, type aliases | `CamelCase` | `ArmyType`, `DeedType` |
| functions and methods | `CamelCase`, a verb for actions | `RecordDeed()`, `FindControl()` |
| variables, parameters | `lower_case` | `defending_side`, `tile_x` |
| class data members | `lower_case_` (trailing underscore) | `armies_` |
| plain struct fields | `lower_case` | `city.owner_index` |
| constants, `constexpr`, enumerators | `kCamelCase` | `kMaxDeedsPerRound`, `DeedType::kCityWon` |
| namespaces | `lower_case` | `w2::history` |
| macros (avoid) | `UPPER_CASE` | |

**Names say what they hold.** At least three letters
(`readability-identifier-length`); the only short names allowed are `i`/`j`
as loop counters, `x`/`y`/`dx`/`dy` as coordinates and `n` as a count.
No private abbreviations: `game` not `g`, `side` not `s`, `city` not `c`,
`attacker` not `a`, `screen_x` not `sx`. A name that needs a comment to
explain it is the wrong name.

A function's name must not equal a type's: give it a verb (`BattleLines()`
returns a `Lines`).

## Comments

**Doxygen, explaining first.** Every class, struct, enum and function that is
not trivially obvious from its name opens with a `/** */` block: what it is
or does, and *why*, in plain English. Members and enumerators get a trailing
`///<`.

```cpp
/**
 * @brief A stack of up to eight armies standing on one tile.
 *
 * Owns the order the armies fight in. Moving the stack moves all of them.
 */
struct Stack {
    std::vector<Army*> armies;  ///< In fighting order, weakest first.
    TilePos position;           ///< Where the stack stands.
};

/**
 * @brief Moves the stack one step along its route.
 * @param stack The stack to move; it must have moves left.
 * @return The step taken, or std::nullopt when it is out of moves.
 */
std::optional<TilePos> Step(Stack& stack);
```

- `@param` and `@return` when they add something the names don't say.
- Inside functions, comments explain *why*, not *what*; good names cover the
  what.
- **No addresses of the original program** (`6d51:1244`, `DS:0e54`) in C++
  comments. The trail back to the 1993 game runs through the reference: the
  C++ matches the Lua (`compare.sh`), and the Lua, `docs/re/` and
  `decompiled/` keep the addresses.
- Behaviour that is required to match the reference but looks wrong gets a
  plain-English note: `// Matches the reference: razed cities count here
  too.`
- File formats are documented in `docs/formats/`, the rules in
  `docs/rules.md`; comments may point there (`see docs/formats/scn.md`).

## Casts, types and literals

- `static_cast` / `reinterpret_cast`, never C-style casts.
- Upper-case literal suffixes: `1.0F`, `640UZ`.
- A multiplication that is widened is done in the wide type:
  `static_cast<size_t>(width) * height`.
- Single-argument constructors are `explicit`, unless implicit conversion is
  the point (then `NOLINT(google-explicit-constructor)` with the reason).
- A `switch` over an `enum class` lists every value and has no `default`,
  so the compiler (`-Wswitch`) flags a value someone forgot.

## Tools

| | |
|---|---|
| Build everything | `bazel build //cpp/...` |
| Play | `bazel run //cpp:play [-- --scenario ERYTHEA --window]` |
| clangd / clang-tidy database | `python3 tools/compile_commands.py` |
| clang-tidy, counted per check | `sh tools/tidy.sh [path-regex]` |
| Sanitizers | `bazel test --config=asan //cpp/...` |
| A zip to share | `sh tools/package.sh` |
