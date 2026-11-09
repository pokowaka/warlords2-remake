/**
 * @file
 * @brief Persistent JSON-backed user preferences store (`w2::PreferencesStore`).
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "absl/status/status.h"

#include "nlohmann/json.hpp"

namespace w2 {

/**
 * @brief Key-value store for remake-specific user settings persisted as JSON on disk.
 *
 * Lazily loads `file_path_` on first read or write and normalizes string,
 * integer, and boolean JSON fields into canonical string representations.
 * Missing or malformed files initialize an empty store without throwing.
 * `Set` mutates in-memory settings (removing keys assigned `""`), and `Save`
 * writes the serialized JSON object atomically to `file_path_`.
 *
 * Invariants:
 * - Once `loaded_` is true, `values_.is_object()` holds and every stored value
 *   in `values_` is a JSON string.
 *
 * Not thread-safe; accessed exclusively on the main UI thread or within
 * isolated test instances.
 */
class PreferencesStore {
  public:
    /// Default filename inside the user application directory.
    static constexpr std::string_view kDefaultFileName = "w2-prefs.json";

    /**
     * @brief Constructs a store backed by `file_path` (loaded lazily on first access).
     * @param file_path Target JSON file path on disk.
     */
    explicit PreferencesStore(std::filesystem::path file_path);

    PreferencesStore(const PreferencesStore&) = delete;
    PreferencesStore& operator=(const PreferencesStore&) = delete;
    PreferencesStore(PreferencesStore&&) noexcept = default;
    PreferencesStore& operator=(PreferencesStore&&) noexcept = default;
    ~PreferencesStore() = default;

    /**
     * @brief Resolves the directory holding user preferences and save files.
     *
     * Honours the `W2_USER_DIR` environment variable when set (for hermetic
     * test and script runs); otherwise queries `SDL_GetPrefPath`.
     */
    static std::filesystem::path UserDirectory();

    /**
     * @brief Resolves `file_name` inside `UserDirectory()`.
     * @param file_name Leaf filename (e.g. `"w2-prefs.json"` or `"w2-save0.json"`).
     */
    static std::filesystem::path UserFilePath(std::string_view file_name);

    /** @brief Creates a `PreferencesStore` targeting `w2-prefs.json` in `UserDirectory()`. */
    static PreferencesStore ForUserApp();

    /** @brief Returns the backing JSON file path. */
    const std::filesystem::path& FilePath() const { return file_path_; }

    /**
     * @brief Reads setting `key` as a string, or `""` when unset.
     * @param key Setting identifier (e.g. `"scale"`, `"crt"`, `"music"`).
     */
    std::string Get(std::string_view key);

    /**
     * @brief Reads setting `key` as an integer, or `fallback` when unset or non-numeric.
     * @param key Setting identifier (e.g. `"zoom"`).
     * @param fallback Default value returned when `key` cannot be parsed as an integer.
     */
    int GetInt(std::string_view key, int fallback = 0);

    /**
     * @brief Updates setting `key` in memory (`""` removes the key from the store).
     * @param key Setting identifier.
     * @param value New string value, or empty to clear.
     */
    void Set(std::string_view key, std::string_view value);

    /**
     * @brief Updates setting `key` to decimal `value` in memory.
     * @param key Setting identifier.
     * @param value Integer setting value.
     */
    void Set(std::string_view key, int value);

    /**
     * @brief Atomically writes all current settings to `file_path_` as JSON.
     * @return `absl::OkStatus()` on success, or an actionable I/O error status.
     */
    absl::Status Save();

  private:
    void EnsureLoaded();

    std::filesystem::path file_path_;  ///< Backing JSON file path on disk.
    nlohmann::ordered_json values_ =
            nlohmann::ordered_json::object();  ///< In-memory key-value map.
    bool loaded_ = false;                      ///< True once `file_path_` has been inspected.
};

}  // namespace w2
