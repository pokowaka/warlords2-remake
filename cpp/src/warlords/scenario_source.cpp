/**
 * @file
 * @brief Disk and in-memory implementations of `IScenarioSource` for loading
 * Warlords II scenario payload files without process-wide virtual filesystem state.
 */
#include "warlords/scenario_source.hpp"

#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"

#include "util/util.hpp"

namespace w2 {

DiskScenarioSource::DiskScenarioSource(std::filesystem::path scenario_dir,
                                       std::string scenario_name)
        : scenario_dir_(std::move(scenario_dir)), scenario_name_(std::move(scenario_name)) {}

absl::StatusOr<std::vector<uint8_t>> DiskScenarioSource::ReadExtension(std::string_view ext) const {
    std::filesystem::path target = scenario_dir_ / (scenario_name_ + "." + Upper(ext));
    return ReadBytes(target);
}

std::string DiskScenarioSource::DescribeSource() const {
    return (scenario_dir_ / scenario_name_).string();
}

MemoryScenarioSource::MemoryScenarioSource(ScenarioFiles files, std::filesystem::path fallback_dir,
                                           std::string fallback_name)
        : fallback_dir_(std::move(fallback_dir)), fallback_name_(std::move(fallback_name)) {
    for (auto& [ext, bytes] : files) {
        files_[Upper(ext)] = std::move(bytes);
    }
}

absl::StatusOr<std::vector<uint8_t>> MemoryScenarioSource::ReadExtension(
        std::string_view ext) const {
    std::string upper_ext = Upper(ext);
    auto found_it = files_.find(upper_ext);
    if (found_it != files_.end()) return found_it->second;
    if (!fallback_dir_.empty() && !fallback_name_.empty()) {
        return ReadBytes(fallback_dir_ / (fallback_name_ + "." + upper_ext));
    }
    return absl::NotFoundError(absl::StrFormat(
            "Failed to read scenario extension '.%s' from in-memory bundle '%s': key '%s' is "
            "absent and no disk fallback directory was provided; include '%s' in ScenarioFiles.",
            upper_ext, DescribeSource(), upper_ext, upper_ext));
}

std::string MemoryScenarioSource::DescribeSource() const {
    if (!fallback_dir_.empty() && !fallback_name_.empty()) {
        return absl::StrFormat("memory(%s)", (fallback_dir_ / fallback_name_).string());
    }
    return "memory";
}

}  // namespace w2
