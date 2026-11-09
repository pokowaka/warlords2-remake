#include "front/menu.hpp"

#include <algorithm>
#include <cstdio>

namespace menu {

const std::vector<Menu> kMenus = {
    {"SSG", {{"About Warlords II", "", "?"}}},
    {"Game",
     {{"Settings", "alt X", ""},
      {"Shortcuts", "alt U", ""},
      {"-", "", ""},
      {"New game", "alt N", ""},
      {"Save game", "alt S", ""},
      {"Load game", "alt L", ""},
      {"-", "", ""},
      {"Save map", "alt M", ""},
      {"Load map", "alt Z", ""},
      {"-", "", ""},
      {"Quit", "^Q", ""}}},
    {"Order",
     {{"Fight Order", "i", ""},
      {"Move All", "m", ""},
      {"Disband", "q", ""},
      {"Signpost", "x", ""},
      {"-", "", ""},
      {"Resign", "r", ""}}},
    {"Report",
     {{"Army", "a", ""},
      {"City", "k", ""},
      {"Gold", "g", ""},
      {"Production", "n", ""},
      {"Winning", "w", ""},
      {"-", "", ""},
      {"Diplomacy", "d", ""},
      {"-", "", ""},
      {"Quest", "=", ""}}},
    {"Hero",
     {{"Inspect", ",", ""}, {"Plant Flag", "f", ""}, {"Levels", "u", ""}, {"Search", "z", ""}}},
    {"View",
     {{"Army Bonus", "o", ""},
      {"Items", "t", ""},
      {"-", "", ""},
      {"Build", "b", ""},
      {"Cities", "c", ""},
      {"Production", "p", ""},
      {"Vectoring", "v", ""},
      {"Ruins", ".", ""},
      {"Stack", "s", ""}}},
    {"History",
     {{"City", "h", ""},
      {"Events", "e", ""},
      {"Gold", "j", ""},
      {"Winners", "y", ""},
      {"-", "", ""},
      {"Triumphs", "l", ""}}},
    {"Turn", {{"End Turn", "alt E", ""}}},
};

/** @brief A scale as the menu shows it: 1.67, 1.6, 2. */
static std::string ScaleName(double scale) {
    char buffer[16];
    snprintf(buffer, sizeof buffer, "%.2f", scale);
    std::string text = buffer;
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}

std::vector<Menu> WithZooms(int max_ui, int max_map, double smooth_ui, double map_shrink) {
    std::vector<Menu> out;
    for (const Menu& original : kMenus) {
        Menu extended{original.title, {}};
        if (original.title == "Game") {
            for (size_t j = 0; j < original.items.size(); j++) {
                extended.items.push_back(original.items[j]);
                if (j == 1) {
                    extended.items.push_back({"-", "", ""});
                    extended.items.push_back({"AdLib music", "", "music fm"});
                    extended.items.push_back({"MT-32 music", "", "music mt32"});
                    extended.items.push_back({"SC-55 music", "", "music sc55"});
                    extended.items.push_back({"-", "", ""});
                    extended.items.push_back({"Modern AI", "", "ai modern"});
                    extended.items.push_back({"Classic AI", "", "ai classic"});
                }
            }
        } else if (original.title == "View") {
            extended.items = original.items;
            extended.items.push_back({"-", "", ""});
            for (int zoom = 1; zoom <= max_map; zoom++)
                extended.items.push_back({"Map " + ScaleName(zoom * map_shrink) + "x", "",
                                          "map zoom " + std::to_string(zoom)});
            extended.items.push_back({"-", "", ""});
            for (int ui_scale = 1; ui_scale <= max_ui; ui_scale++)
                extended.items.push_back({"Interface " + std::to_string(ui_scale) + "x", "",
                                          "ui scale " + std::to_string(ui_scale)});
            if (smooth_ui > 0)
                extended.items.push_back(
                        {"Interface " + ScaleName(smooth_ui) + "x smooth", "", "ui scale smooth"});
            extended.items.push_back({"-", "", ""});
            extended.items.push_back({"Full screen", "", "screen full"});
            extended.items.push_back({"Window", "", "screen window"});
            extended.items.push_back({"-", "", ""});
            extended.items.push_back({"4:3 (original)", "", "screen 4:3"});
            extended.items.push_back({"Fit available space", "", "screen fit"});
            extended.items.push_back({"-", "", ""});
            extended.items.push_back({"CRT screen", "", "screen crt"});
        } else {
            extended.items = original.items;
        }
        out.push_back(extended);
    }
    return out;
}

/** The width of the tick a menu puts by a setting in use (not the
 *  original's): a diamond in the key column. */
static const int kTickW = 5;
/** @brief True for an item that is a setting, ticked when in use. */
static bool Tickable(const std::string& act) {
    for (const char* prefix : {"map zoom ", "ui scale ", "screen ", "music ", "ai "})
        if (act.rfind(prefix, 0) == 0) return true;
    return false;
}

Layout MakeLayout(const Font& font, int screen_width, const std::vector<Menu>& menus) {
    Layout out;
    int x = kBarX;
    int line_height = font.LineHeight();
    for (const Menu& source : menus) {
        int title_width = (font.Width(source.title) + 7) / 8 * 8;
        int drop_width = 0, key_col = 0, key_width = 0;
        bool ticked = false;
        for (const Item& item : source.items) {
            if (item.label != "-") {
                int row_width = 12 + font.Width(item.label);
                if (!item.key.empty()) {
                    key_col = std::max(key_col, row_width + 5);
                    key_width = std::max(key_width, font.Width(item.key));
                    row_width = key_col + key_width;
                } else if (Tickable(item.act)) {
                    // a setting's tick goes in the key column: keep it clear of the label
                    key_col = std::max(key_col, row_width + 5);
                    ticked = true;
                }
                drop_width = std::max(drop_width, row_width + 5);
            }
        }
        if (ticked) drop_width = std::max(drop_width, key_col + std::max(key_width, kTickW) + 5);
        int y = kBarH + 1;
        Title placed;
        placed.title = source.title;
        placed.x = x;
        placed.w = title_width + 8;
        int row_offset = 2;
        for (const Item& item : source.items) {
            int row_height = item.label == "-" ? 2 : line_height + 2;
            placed.drop.rows.push_back(
                    Row{item.label, item.key, item.act, x, y + row_offset, drop_width, row_height});
            row_offset += row_height;
        }
        int drop_x = std::min(x, screen_width - drop_width);
        for (Row& row : placed.drop.rows) row.x = drop_x;
        placed.drop.x = drop_x;
        placed.drop.y = y;
        placed.drop.w = drop_width;
        placed.drop.h = row_offset + 1;
        placed.drop.key_col = key_col;
        out.push_back(placed);
        x += title_width + 16;
    }
    return out;
}

int TitleAt(const Layout& bar, int x, int y) {
    if (y < 0 || y >= kBarH) return -1;
    for (size_t i = 0; i < bar.size(); i++) {
        if (x >= bar[i].x && x < bar[i].x + bar[i].w) return static_cast<int>(i);
    }
    return -1;
}

const Row* RowAt(const Layout& bar, int index, int x, int y) {
    if (index < 0 || index >= static_cast<int>(bar.size())) return nullptr;
    for (const Row& row : bar[index].drop.rows) {
        if (row.label != "-" && x >= row.x && x < row.x + row.w && y >= row.y && y < row.y + row.h)
            return &row;
    }
    return nullptr;
}

}  // namespace menu
