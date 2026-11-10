/**
 * @file
 * @brief Slim Warlords II CLI entry point: parses `absl::flags` into `w2::ApplicationConfig` and
 * runs `w2::Application::Run`.
 *
 *     warlords2 [--data DIR] [--scenario NAME] [--seed N] [--window]
 *               [--size WxH] [--script FILE]
 *
 * Run from the repository root, or point --data at your copy of the game.
 */
#include <string>

#include "absl/flags/flag.h"

#include "front/application.hpp"
#include "util/util.hpp"

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
ABSL_FLAG(std::string, data, "original", "Path to the Warlords II game data directory.");
ABSL_FLAG(std::string, scenario, "", "Scenario directory name to launch directly.");
ABSL_FLAG(double, seed, 0.0, "Random seed when launching --scenario directly.");
ABSL_FLAG(bool, window, false, "Open in windowed mode.");
ABSL_FLAG(bool, fullscreen, false, "Open in fullscreen mode.");
ABSL_FLAG(std::string, script, "", "Automated UI test script file to execute.");
ABSL_FLAG(std::string, size, "", "Initial window dimensions formatted as WxH.");
ABSL_FLAG(bool, classic_ai, false, "Default computer sides to the 1993 Classic AI.");
ABSL_FLAG(bool, modern_ai, false, "Default computer sides to the Modern neural AI.");
ABSL_FLAG(bool, modern_ui, false, "Enable modern UI overlays and AI Brain Inspector.");
ABSL_FLAG(bool, classic_ui, false, "Disable modern UI overlays.");
ABSL_FLAG(std::string, weights, "", "Path to Modern AI policy weights JSON.");
ABSL_FLAG(bool, diplomacy, false, "Force the Diplomacy game option enabled.");
ABSL_FLAG(bool, hidden_map, false, "Force the Hidden Map game option enabled.");
ABSL_FLAG(bool, military_advisor, false, "Force the Military Advisor option enabled.");
ABSL_FLAG(bool, no_military_advisor, false, "Force the Military Advisor option disabled.");
ABSL_FLAG(bool, spectate, false, "Run all sides as computer players (AI-vs-AI spectator mode).");
ABSL_FLAG(bool, all_ai, false, "Alias for --spectate.");
ABSL_FLAG(bool, personalities, false, "Force 27-card evolved AI personalities enabled.");
ABSL_FLAG(bool, no_personalities, false, "Force 27-card evolved AI personalities disabled.");
ABSL_FLAG(bool, warlord_lookahead, false,
          "Enable 1-ply Warlord tactical lookahead against the Imperial anchor.");
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

int main(int argc, char** argv) {
    w2::ParseCliFlags(argc, argv,
                      "warlords2 [--data DIR] [--scenario NAME] [--seed N] [--window] [--size WxH] "
                      "[--script FILE]");

    w2::ApplicationConfig config;
    std::filesystem::path data_dir = absl::GetFlag(FLAGS_data);
    std::filesystem::path weights_file = absl::GetFlag(FLAGS_weights);
    std::filesystem::path script_file = absl::GetFlag(FLAGS_script);
    if (const char* workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
        workspace != nullptr && *workspace != '\0') {
        if (data_dir.is_relative()) data_dir = std::filesystem::path(workspace) / data_dir;
        if (!weights_file.empty() && weights_file.is_relative()) {
            weights_file = std::filesystem::path(workspace) / weights_file;
        }
        if (!script_file.empty() && script_file.is_relative()) {
            script_file = std::filesystem::path(workspace) / script_file;
        }
    }
    config.data_dir = data_dir;
    config.weights_file = weights_file;
    config.script_file = script_file;
    config.scenario = w2::Upper(absl::GetFlag(FLAGS_scenario));
    config.seed = absl::GetFlag(FLAGS_seed);
    config.window_size = absl::GetFlag(FLAGS_size);
    config.force_window = absl::GetFlag(FLAGS_window);
    config.force_fullscreen = absl::GetFlag(FLAGS_fullscreen);
    config.force_classic_ai = absl::GetFlag(FLAGS_classic_ai);
    config.force_modern_ai = absl::GetFlag(FLAGS_modern_ai);
    config.modern_ui = absl::GetFlag(FLAGS_modern_ui) && !absl::GetFlag(FLAGS_classic_ui);
    config.force_diplomacy = absl::GetFlag(FLAGS_diplomacy);
    config.force_hidden_map = absl::GetFlag(FLAGS_hidden_map);
    if (absl::GetFlag(FLAGS_military_advisor)) config.military_advisor_override = 1;
    if (absl::GetFlag(FLAGS_no_military_advisor)) config.military_advisor_override = 0;
    config.spectate_all_ai = absl::GetFlag(FLAGS_spectate) || absl::GetFlag(FLAGS_all_ai);
    if (absl::GetFlag(FLAGS_personalities)) config.force_personalities = 1;
    if (absl::GetFlag(FLAGS_no_personalities)) config.force_personalities = 0;
    config.warlord_lookahead = absl::GetFlag(FLAGS_warlord_lookahead);

    return w2::Application::Run(config);
}
