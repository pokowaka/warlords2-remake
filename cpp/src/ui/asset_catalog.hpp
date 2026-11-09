/**
 * @file
 * @brief Encapsulated catalog of Warlords II palette, `.PCK` textures, UI chrome, and bitmap fonts
 * (`w2::AssetCatalog`).
 */
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

#include "front/font.hpp"
#include "front/pckimage.hpp"
#include "front/screen.hpp"
#include "platform/gfx.hpp"
#include "warlords/pal.hpp"

namespace w2 {

/**
 * @brief Owns the active colour palette, decoded `.PCK` GPU textures, `screen::Screen`
 * layout/chrome, and proportional bitmap fonts (`TEXT`, `CHANCE17`, `CHANCE36`) for a loaded UI
 * session.
 *
 * Loads eager textures and fonts via `Load()` and maintains lazy per-instance caches for
 * side `MOVEBAR<n>.PCK` strips, `infobox` popup boards, and dialog-specific `.PCK` art so
 * dialog modules hold zero module-static `gfx::ImageP` globals.
 *
 * Invariants:
 * - After `Load()` returns `absl::OkStatus()`, `screen_`, `font_`, `big_font_`, `title_font_`,
 *   and all core `.PCK` texture slots are non-null.
 *
 * Thread safety: Not thread-safe; accessed exclusively on the main UI rendering thread.
 */
class AssetCatalog {
  public:
    using TerrainSheets = std::array<gfx::ImageP, 2>;
    using SideArmySheets = std::array<gfx::ImageP, 9>;
    using MedalSheets = std::array<gfx::ImageP, 4>;
    using IntImageMap = absl::flat_hash_map<int, gfx::ImageP>;
    using KeyedImageMap = absl::flat_hash_map<std::string, gfx::ImageP>;

    AssetCatalog() = default;
    ~AssetCatalog() = default;

    AssetCatalog(const AssetCatalog&) = delete;
    AssetCatalog& operator=(const AssetCatalog&) = delete;

    /**
     * @brief Loads the palette, `.PCK` sprite sheets, main `screen::Screen`, and 3 `Font` handles.
     * @param renderer Active 2D renderer used to upload RGBA textures.
     * @param data_dir Root Warlords II asset directory containing `DATA/`, `PICS/`, and `TERRAIN*`.
     * @param terrain Terrain set index (`0` for standard chrome/default tiles).
     * @return `absl::OkStatus()` on success, or an actionable error status if an asset is missing.
     */
    absl::Status Load(gfx::Renderer& renderer, const std::filesystem::path& data_dir,
                      int terrain = 0);

    /**
     * @brief Applies serialized shortcut-slot mappings (`JSON` integer array) onto `screen_->ui`.
     * @param shortcuts_json Serialized JSON array string from `PreferencesStore`, or empty.
     */
    void ApplySavedShortcuts(std::string_view shortcuts_json);

    const std::filesystem::path& DataDir() const { return data_dir_; }
    void SetDataDir(std::filesystem::path data_dir) { data_dir_ = std::move(data_dir); }

    const pal::Palette& Palette() const { return palette_; }
    int TerrainIndex() const { return terrain_; }

    screen::Screen& MainScreen() { return *screen_; }
    const screen::Screen& MainScreen() const { return *screen_; }
    screen::Screen* MainScreenPtr() { return screen_.get(); }
    const screen::Screen* MainScreenPtr() const { return screen_.get(); }

    const gfx::ImageP& Sheet(int index) const { return sheets_[index <= 0 ? 0 : 1]; }
    const gfx::ImageP& RoadImg() const { return road_img_; }
    const gfx::ImageP& ShadowImg() const { return shadow_img_; }
    const gfx::ImageP& Marble() const { return marble_; }
    const gfx::ImageP& BigArmy() const { return big_army_; }
    const gfx::ImageP& CityPic() const { return city_pic_; }
    const gfx::ImageP& WarPic() const { return war_pic_; }
    const gfx::ImageP& ShieldImg() const { return shield_img_; }
    const gfx::ImageP& VictoryPic() const { return victory_pic_; }
    const gfx::ImageP& HeroPicMale() const { return hero_pic_m_; }
    const gfx::ImageP& HeroPicFemale() const { return hero_pic_f_; }
    const gfx::ImageP& ArmyImg(int side) const;
    const gfx::ImageP& AtransShields() const { return atrans_shields_; }
    const gfx::ImageP& FogImg() const { return fog_img_; }
    const gfx::ImageP& CursImg() const { return curs_img_; }
    const gfx::ImageP& Abits() const { return abits_; }
    const gfx::ImageP& PointerImg() const { return pointer_img_; }
    const gfx::ImageP& ShieldsImg() const { return shields_img_; }
    const gfx::ImageP& CityBack() const { return city_back_; }
    const gfx::ImageP& SearchPic() const { return search_pic_; }
    const gfx::ImageP& TemplePic() const { return temple_pic_; }
    const gfx::ImageP& ScrollPic() const { return scroll_pic_; }
    gfx::ImageP LevelPic(int level) const;
    const gfx::ImageP& MedalPic(int index) const;

    /** @brief Lazily loads and caches `TERRAIN<n>/MOVEBAR<side_index>.PCK`. */
    gfx::ImageP MoveBar(gfx::Renderer& renderer, int side_index);

    /** @brief Lazily loads and caches an arbitrary `.PCK` file keyed by `(path, key)`. */
    gfx::ImageP CachedImage(gfx::Renderer& renderer, const std::filesystem::path& path,
                            int key = pckimage::kNokey);

    /** @brief Lazily loads and caches an `infobox` board image (`PICS/<file>`). */
    gfx::ImageP BoardImage(gfx::Renderer& renderer, int bitmap_id, std::string_view file, int key);

    const Font& TextFont() const { return *font_; }
    const Font& BigFont() const { return *big_font_; }
    const Font& TitleFont() const { return *title_font_; }
    const FontP& BigFontPtr() const { return big_font_; }

    /** @brief Returns a UI font by 1993 index: `1` = `CHANCE36`, `2` = `CHANCE17`, else `TEXT`. */
    const Font& GetFont(int font_index) const;

  private:
    std::filesystem::path data_dir_;  ///< Root Warlords II game data directory.
    pal::Palette palette_{};          ///< Loaded 16-colour UI/terrain palette.
    int terrain_ = 0;                 ///< Active terrain directory index (`TERRAIN<n>`).
    TerrainSheets sheets_{};          ///< `SCENERY0.PCK` and `SCENERY1.PCK` terrain sheets.
    gfx::ImageP road_img_;            ///< `ROAD.PCK` road and encampment tower sheet.
    gfx::ImageP shadow_img_;          ///< `ASHADOW.PCK` army silhouettes and route rings.
    gfx::ImageP marble_;              ///< `MARBLE.PCK` popup background texture.
    gfx::ImageP big_army_;            ///< `BIGARMY.PCK` city-production portraits.
    gfx::ImageP city_pic_;            ///< `CITY.PCK` start-of-turn banner backdrop.
    gfx::ImageP war_pic_;             ///< `WAR.PCK` battle cloud and water strip.
    gfx::ImageP shield_img_;          ///< `BSHIELD.PCK` battle and city small shields.
    gfx::ImageP victory_pic_;         ///< `VICTORY.PCK` city-capture backdrop.
    gfx::ImageP hero_pic_m_;          ///< `MHERO.PCK` male hero recruitment portrait.
    gfx::ImageP hero_pic_f_;          ///< `FHERO.PCK` female hero recruitment portrait.
    SideArmySheets army_img_{};       ///< `A0.PCK..A8.PCK` faction and neutral army sheets.
    gfx::ImageP atrans_shields_;      ///< `ATRANS2.PCK` map shields, markers, and hero figures.
    gfx::ImageP fog_img_;             ///< `HIDDEN.PCK` fog-of-war transition cells.
    gfx::ImageP curs_img_;            ///< `CURS.PCK` animated stack selection box.
    gfx::ImageP abits_;               ///< `ABITS.PCK` rings, digits, checkboxes, and icons.
    gfx::ImageP pointer_img_;         ///< `STAND.PCK` 12-cursor sheet.
    gfx::ImageP shields_img_;         ///< `SHIELDS.PCK` 40x40 shields and bar tiles.
    gfx::ImageP city_back_;           ///< `CITYBACK.PCK` city backdrop.
    gfx::ImageP search_pic_;          ///< `SEARCH.PCK` ruin search picture.
    gfx::ImageP temple_pic_;          ///< `TEMPLE.PCK` temple blessing picture.
    gfx::ImageP scroll_pic_;          ///< `SCROLL.PCK` message scroll backdrop.
    IntImageMap level_pic_;           ///< `LEVEL2..4.PCK` hero promotion pictures.
    MedalSheets medal_pic_{};         ///< `MEDAL0..3.PCK` combat medal pictures.
    IntImageMap move_bars_;           ///< Lazy `MOVEBAR<n>.PCK` progress strips per side.
    IntImageMap boards_;              ///< Lazy `POPUP*.PCK` right-click infobox boards.
    KeyedImageMap cached_images_;     ///< Lazy path+key `.PCK` texture cache for dialogs.
    screen::ScreenHandle screen_;     ///< Main screen layout, control bitmaps, and `uidata::UI`.
    FontP font_;                      ///< `TEXT` small proportional font.
    FontP big_font_;                  ///< `CHANCE17` medium proportional font.
    FontP title_font_;                ///< `CHANCE36` large heading font.
};

}  // namespace w2
