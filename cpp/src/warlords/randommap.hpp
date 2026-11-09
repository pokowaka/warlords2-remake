/**
 * @file
 * @brief The random map generator, "A Random World": setup and phased map
 *        generation (docs/re/random_map.md).
 *
 * A port of the web port's web/src/warlords/randommap.js, line for line: the
 * engines' dice are the same, so a seed makes the same world in all three.
 * The result is a scenario like any other -- RANDOM.SCN (Erythea's,
 * rewritten), .MAP, .RD, .SGN, .CTY and .SPC -- returned as an in-memory
 * `ScenarioFiles` bundle read via `MemoryScenarioSource` (`scn::LoadFromFiles`).
 * The shipped RANDOM folder holds the last world the original made, and is
 * left alone.
 */
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>

#include "absl/status/statusor.h"

#include "warlords/rng.hpp"
#include "warlords/scenario_source.hpp"
#include "warlords/scengen.hpp"

namespace w2::randommap {

/** @brief Directory and base name of the generated random scenario. */
constexpr const char* kDir = "RANDOM";
/** @brief File extensions produced by the random map generator. */
extern const char* const kFiles[6];
/** @brief Generated scenario file contents keyed by extension. */
using Files = ScenarioFiles;

/** @brief Values shown beside each slider in the random map setup menu. */
extern const int kSliderShows[4][7];
/** @brief Printf format strings for each slider's value display. */
extern const char* const kSliderFormats[4];
/** @brief Slider value meaning "Random" (rolled as 1d7-1). */
constexpr int kRandomSlider = 7;

/** @brief Configuration options for generating a random world. */
struct Options {
    std::filesystem::path data_dir;            ///< Path to the game data directory.
    Rng* rng = nullptr;                        ///< Random number generator to draw from.
    std::array<int, 4> sliders{{3, 3, 2, 3}};  ///< Water, Hills, Cities, Forest sliders (0-7).
    bool allies = false;                       ///< Whether cities may produce magical ally armies.
    int terrain_set = 0;                       ///< Terrain graphics set index.
    /**
     * @brief Not the original's: choose the eight capitals for a fair start
     *        (far apart, every city near some capital, each side as much room
     *        to grow as the next) instead of the classic generator's random
     *        picks. Off, the world is the original's, byte for byte. See
     *        docs/mapeval.md.
     */
    bool fair_capitals = false;
    /**
     * @brief Not the original's: make the world with the scenario generator
     *        (warlords/scengen) instead, shaped by `world`; its capitals are
     *        always the fair ones, its city names never repeat, and its .SCN
     *        reads as an ordinary scenario, not a random one.
     */
    bool scenario = false;
    scengen::Settings world;  ///< Scenario generator parameters when `scenario` is true.
};

/**
 * @brief Resolves random sliders: any slider set to `kRandomSlider` (7) is rolled as 1d7-1.
 * @param sliders Raw slider selections (`0..7`) for Water, Hills, Cities, and Forest.
 * @param rng Dice sequence to draw resolved slider values from.
 * @return Resolved slider indices in `0..6`.
 */
std::array<int, 4> Settle(std::array<int, 4> sliders, Rng& rng);

class Generator;

/**
 * @brief Incremental driver that builds a random world one progress-bar stage at a time.
 *
 * Callers pull generation forward by invoking `Step()` on each UI frame or loop iteration,
 * reading `Progress()` (`0..100`) to render the progress bar until `Step()` returns `false`,
 * at which point `GetFiles()` holds the six scenario files (`SCN`, `MAP`, `RD`, `SGN`,
 * `CTY`, `SPC`).
 *
 * Invariants:
 * - `gen_` is non-null, `0 <= pct_ <= 100`, and once `Step()` returns `false`, `pct_ == 100`
 *   and `files_` contains all six scenario payloads.
 *
 * Thread safety: Not thread-safe; drive a `Run` instance from a single thread.
 *
 * Ownership & lifetimes: Exclusively owns the underlying `Generator`. The `Rng` pointed to by
 * `Options::rng` is unowned and must outlive all `Step()` calls on this `Run`.
 */
class Run {
  public:
    /**
     * @brief Creates a phased generator run, loading `RANDOM.DAT`, `ERYTHEA.SCN`, and army types.
     * @param opts World generation parameters; `opts.rng` must be non-null and `opts.data_dir`
     *        must contain `RANDOM/RANDOM.DAT`, `ERYTHEA/ERYTHEA.SCN`, and `TERRAIN0/ARMYTYPE.DAT`.
     * @return Initialized `Run` ready for `Step()`, or an actionable error status on invalid
     *         arguments or missing/corrupt template data.
     */
    static absl::StatusOr<std::unique_ptr<Run>> Create(const Options& opts);
    ~Run();
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    /**
     * @brief Executes the next generation phase and updates `Progress()`.
     * @return `true` while generation is still in progress (including the 100% completion step);
     *         `false` once all phases have already completed.
     */
    bool Step();
    /** @brief How far along generation is, 0-100, as the original's progress bar shows it. */
    int Progress() const { return pct_; }
    /** @brief The world's files, keyed by extension, once `Step()` has returned `false`. */
    const Files& GetFiles() const { return files_; }

  private:
    explicit Run(std::unique_ptr<Generator> gen);

    std::unique_ptr<Generator> gen_;  ///< Underlying generator state machine.
    int pct_ = 0;                     ///< Progress percentage (0-100).
    Files files_;                     ///< Output scenario files once complete.
};

/**
 * @brief Generates a complete random world synchronously across all phases.
 * @param opts World generation parameters (`opts.rng` must be non-null).
 * @return Generated in-memory scenario file bundle, or an actionable error status.
 */
absl::StatusOr<Files> GenerateNow(const Options& opts);

/**
 * @brief Reads the display name of terrain graphics set `set` from `DATA/TERRAIN.DAT`.
 * @param data_dir Root directory containing `DATA/TERRAIN.DAT`.
 * @param set Zero-based terrain set index.
 * @return Terrain set display name, or an empty string if unreadable or out of range.
 */
std::string TerrainSetName(const std::filesystem::path& data_dir, int set);

/**
 * @brief Returns how many terrain sets are defined in `DATA/TERRAIN.DAT`.
 * @param data_dir Root directory containing `DATA/TERRAIN.DAT`.
 * @return Number of terrain sets (at least 1).
 */
int TerrainSets(const std::filesystem::path& data_dir);

}  // namespace w2::randommap
