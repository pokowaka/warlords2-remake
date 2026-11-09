#include "ui/asset_catalog.hpp"

#include <algorithm>
#include <utility>

#include "nlohmann/json.hpp"
#include "warlords/pck.hpp"
#include "warlords/uidata.hpp"

namespace w2 {

absl::Status AssetCatalog::Load(gfx::Renderer& renderer, const std::filesystem::path& data_dir,
                                int terrain) {
    data_dir_ = data_dir;
    terrain_ = terrain;
    move_bars_.clear();
    boards_.clear();
    cached_images_.clear();

    std::string terrain_dir = "TERRAIN" + std::to_string(terrain);
    auto loaded_pal = pal::Load(data_dir_ / terrain_dir / "WAR2.PAL");
    if (!loaded_pal.ok()) return loaded_pal.status();
    palette_ = *loaded_pal;

    auto load_into = [&](gfx::ImageP& dest, const std::filesystem::path& rel_path,
                         int key = pckimage::kNokey) -> absl::Status {
        auto img = pckimage::Load(renderer, data_dir_ / rel_path, palette_, key);
        if (!img.ok()) return img.status();
        dest = std::move(*img);
        return absl::OkStatus();
    };

    for (int i = 0; i < 2; i++) {
        absl::Status status =
                load_into(sheets_[i], terrain_dir + "/SCENERY" + std::to_string(i) + ".PCK");
        if (!status.ok()) return status;
    }
    if (absl::Status status = load_into(road_img_, terrain_dir + "/ROAD.PCK", 1); !status.ok()) {
        return status;
    }
    for (int i = 0; i <= 8; i++) {
        absl::Status status = load_into(
                army_img_[i], terrain_dir + "/A" + std::to_string(i) + ".PCK", pckimage::kCorner);
        if (!status.ok()) return status;
    }
    if (absl::Status status = load_into(shadow_img_, terrain_dir + "/ASHADOW.PCK", 15);
        !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(marble_, "PICS/MARBLE.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(big_army_, "PICS/BIGARMY.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(city_pic_, "PICS/CITY.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(war_pic_, "PICS/WAR.PCK", 1); !status.ok()) return status;
    if (absl::Status status = load_into(shield_img_, terrain_dir + "/BSHIELD.PCK", 12);
        !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(victory_pic_, "PICS/VICTORY.PCK"); !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(hero_pic_m_, "PICS/MHERO.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(hero_pic_f_, "PICS/FHERO.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(atrans_shields_, terrain_dir + "/ATRANS2.PCK", 1);
        !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(fog_img_, "PICS/HIDDEN.PCK", 1); !status.ok())
        return status;

    auto curs_px = pck::Decode(data_dir_ / "PICS/CURS.PCK");
    if (!curs_px.ok()) return curs_px.status();
    curs_img_ = pckimage::FromPixels(renderer, **curs_px, palette_, {{1, 15}, {4, 0}}, {0});

    if (absl::Status status = load_into(abits_, "PICS/ABITS.PCK"); !status.ok()) return status;
    if (absl::Status status = load_into(pointer_img_, "STAND.PCK", 10); !status.ok()) return status;

    auto loaded_screen = screen::Load(renderer, data_dir_, palette_, uidata::kMainScreen, terrain);
    if (!loaded_screen.ok()) return loaded_screen.status();
    screen_ = std::move(*loaded_screen);

    auto text_font = Font::Load(renderer, data_dir_, "TEXT", palette_, 3);
    if (!text_font.ok()) return text_font.status();
    font_ = std::move(*text_font);

    auto chance17 = Font::Load(renderer, data_dir_, "CHANCE17", palette_, 3);
    if (!chance17.ok()) return chance17.status();
    big_font_ = std::move(*chance17);

    auto chance36 = Font::Load(renderer, data_dir_, "CHANCE36", palette_, 3);
    if (!chance36.ok()) return chance36.status();
    title_font_ = std::move(*chance36);

    if (absl::Status status = load_into(shields_img_, terrain_dir + "/SHIELDS.PCK"); !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(city_back_, terrain_dir + "/CITYBACK.PCK"); !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(search_pic_, "PICS/SEARCH.PCK"); !status.ok()) {
        return status;
    }
    if (absl::Status status = load_into(temple_pic_, "PICS/TEMPLE.PCK"); !status.ok()) {
        return status;
    }
    for (int i = 2; i <= 4; i++) {
        gfx::ImageP level_image;
        absl::Status status = load_into(level_image, "PICS/LEVEL" + std::to_string(i) + ".PCK");
        if (!status.ok()) return status;
        level_pic_[i] = std::move(level_image);
    }
    for (int i = 0; i < 4; i++) {
        absl::Status status = load_into(medal_pic_[i], "PICS/MEDAL" + std::to_string(i) + ".PCK");
        if (!status.ok()) return status;
    }
    return load_into(scroll_pic_, "PICS/SCROLL.PCK", 10);
}

void AssetCatalog::ApplySavedShortcuts(std::string_view shortcuts_json) {
    if (!screen_ || shortcuts_json.empty()) return;
    auto shortcut_list =
            nlohmann::ordered_json::parse(shortcuts_json, nullptr, /*allow_exceptions=*/false);
    if (!shortcut_list.is_discarded() && shortcut_list.is_array()) {
        for (size_t i = 0; i < shortcut_list.size(); i++) {
            if (shortcut_list[i].is_number_integer()) {
                screen_->ui.shortcuts[static_cast<int>(i)] = shortcut_list[i].get<int>();
            }
        }
    }
}

const gfx::ImageP& AssetCatalog::ArmyImg(int side) const {
    if (side < 0 || side == 15 || side > 8) side = 8;
    return army_img_[static_cast<size_t>(side)];
}

gfx::ImageP AssetCatalog::LevelPic(int level) const {
    auto found = level_pic_.find(level);
    return found != level_pic_.end() ? found->second : nullptr;
}

const gfx::ImageP& AssetCatalog::MedalPic(int index) const {
    int clamped = std::clamp(index, 0, 3);
    return medal_pic_[static_cast<size_t>(clamped)];
}

gfx::ImageP AssetCatalog::MoveBar(gfx::Renderer& renderer, int side_index) {
    auto cached = move_bars_.find(side_index);
    if (cached != move_bars_.end()) return cached->second;
    std::string terrain_dir = "TERRAIN" + std::to_string(terrain_);
    auto loaded = pckimage::Load(
            renderer, data_dir_ / terrain_dir / ("MOVEBAR" + std::to_string(side_index) + ".PCK"),
            palette_, pckimage::kCorner);
    gfx::ImageP image = loaded.ok() ? std::move(*loaded) : nullptr;
    move_bars_[side_index] = image;
    return image;
}

gfx::ImageP AssetCatalog::CachedImage(gfx::Renderer& renderer, const std::filesystem::path& path,
                                      int key) {
    std::string cache_key = path.string() + "#" + std::to_string(key);
    auto cached = cached_images_.find(cache_key);
    if (cached != cached_images_.end()) return cached->second;
    auto loaded = pckimage::Load(renderer, path, palette_, key);
    gfx::ImageP image = loaded.ok() ? std::move(*loaded) : nullptr;
    cached_images_.emplace(std::move(cache_key), image);
    return image;
}

gfx::ImageP AssetCatalog::BoardImage(gfx::Renderer& renderer, int bitmap_id, std::string_view file,
                                     int key) {
    auto cached = boards_.find(bitmap_id);
    if (cached != boards_.end()) return cached->second;
    auto loaded = pckimage::Load(renderer, data_dir_ / "PICS" / file, palette_, key);
    if (!loaded.ok() && screen_) {
        auto name = screen_->ui.bitmaps.find(bitmap_id);
        if (name != screen_->ui.bitmaps.end()) {
            loaded = pckimage::Load(renderer, data_dir_ / name->second, palette_, key);
        }
    }
    gfx::ImageP image = loaded.ok() ? std::move(*loaded) : nullptr;
    boards_[bitmap_id] = image;
    return image;
}

const Font& AssetCatalog::GetFont(int font_index) const {
    if (font_index == 1) return *title_font_;
    if (font_index == 2) return *big_font_;
    return *font_;
}

}  // namespace w2
