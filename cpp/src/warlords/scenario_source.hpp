/**
 * @file
 * @brief Scenario payload source abstraction (`IScenarioSource`) and disk/memory
 * implementations for loading `.SCN`, `.MAP`, `.RD`, `.ITM`, `.CTY`, `.SPC`, and `.SGN` files.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

#include "warlords/model/map.hpp"

namespace w2 {

/**
 * @brief Read-only provider for the constituent files of a single Warlords II scenario.
 *
 * Supplies raw binary or text file payloads by upper-case extension (such as
 * `"SCN"`, `"MAP"`, `"RD"`, `"ITM"`, `"CTY"`, `"SPC"`, `"SGN"`) so scenario
 * decoders operate identically over on-disk folders and generated in-memory
 * worlds without process-wide virtual filesystem state.
 *
 * Thread Safety: Implementations are immutable after construction and safe for
 * concurrent `ReadExtension` calls across threads.
 *
 * Ownership & Lifetimes: Callers pass `const IScenarioSource&` into `scn::LoadFromSource`;
 * returned byte vectors are owned by value by the caller.
 */
class IScenarioSource {
  public:
    virtual ~IScenarioSource() = default;

    /**
     * @brief Reads the scenario payload for extension `ext`.
     * @param ext Upper-case extension without a leading dot (e.g. `"SCN"`).
     * @return Raw file bytes, or `absl::NotFoundError` / I/O error status.
     */
    virtual absl::StatusOr<std::vector<uint8_t>> ReadExtension(std::string_view ext) const = 0;

    /** @brief Human-readable identifier of the backing source for error diagnostics. */
    virtual std::string DescribeSource() const = 0;
};

/**
 * @brief `IScenarioSource` backed by `<scenario_dir>/<scenario_name>.<EXT>` on disk.
 *
 * Resolves DOS file names case-insensitively on the host filesystem.
 *
 * Invariants: `scenario_name_` is non-empty for valid scenario directories.
 * Thread Safety: Immutable after construction; thread-safe.
 */
class DiskScenarioSource final : public IScenarioSource {
  public:
    /**
     * @brief Constructs a disk source reading `<scenario_dir>/<scenario_name>.<EXT>`.
     * @param scenario_dir Directory containing the scenario's files.
     * @param scenario_name File stem of the scenario (e.g. `"ERYTHEA"`).
     */
    DiskScenarioSource(std::filesystem::path scenario_dir, std::string scenario_name);

    absl::StatusOr<std::vector<uint8_t>> ReadExtension(std::string_view ext) const override;
    std::string DescribeSource() const override;

  private:
    std::filesystem::path scenario_dir_;  ///< Directory containing the scenario files.
    std::string scenario_name_;           ///< Scenario stem without extension.
};

/**
 * @brief `IScenarioSource` backed by an explicit in-memory `ScenarioFiles` bundle,
 * with optional fallback to an on-disk scenario directory (e.g. `RANDOM/RANDOM.ITM`).
 *
 * Invariants: Keys in `files_` are normalized to upper-case ASCII extensions.
 * Thread Safety: Immutable after construction; thread-safe.
 */
class MemoryScenarioSource final : public IScenarioSource {
  public:
    /**
     * @brief Constructs an in-memory source with optional disk fallback for absent extensions.
     * @param files Map from extension (`"SCN"`, `"MAP"`, ...) to raw bytes.
     * @param fallback_dir Optional disk directory queried when an extension is not in `files`.
     * @param fallback_name Optional file stem inside `fallback_dir`.
     */
    explicit MemoryScenarioSource(ScenarioFiles files, std::filesystem::path fallback_dir = {},
                                  std::string fallback_name = {});

    absl::StatusOr<std::vector<uint8_t>> ReadExtension(std::string_view ext) const override;
    std::string DescribeSource() const override;

    /** @brief Returns the normalized in-memory extension map. */
    const ScenarioFiles& Files() const { return files_; }

  private:
    ScenarioFiles files_;                 ///< In-memory payloads keyed by upper-case extension.
    std::filesystem::path fallback_dir_;  ///< Optional fallback directory on disk.
    std::string fallback_name_;           ///< Optional fallback stem on disk.
};

}  // namespace w2
