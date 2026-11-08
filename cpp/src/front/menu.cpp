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

// A scale as the menu shows it: 1.67, 1.6, 2.
static std::string ScaleName(double s) {
    char b[16];
    snprintf(b, sizeof b, "%.2f", s);
    std::string t = b;
    while (t.back() == '0') t.pop_back();
    if (t.back() == '.') t.pop_back();
    return t;
}

std::vector<Menu> WithZooms(int max_ui, int max_map, double smooth_ui, double map_shrink) {
    std::vector<Menu> out;
    for (const Menu& m : kMenus) {
        Menu n{m.title, {}};
        if (m.title == "Game") {
            for (size_t j = 0; j < m.items.size(); j++) {
                n.items.push_back(m.items[j]);
                if (j == 1) {
                    n.items.push_back({"-", "", ""});
                    n.items.push_back({"AdLib music", "", "music fm"});
                    n.items.push_back({"MT-32 music", "", "music mt32"});
                    n.items.push_back({"SC-55 music", "", "music sc55"});
                    n.items.push_back({"-", "", ""});
                    n.items.push_back({"Modern AI", "", "ai modern"});
                    n.items.push_back({"Classic AI", "", "ai classic"});
                }
            }
        } else if (m.title == "View") {
            n.items = m.items;
            n.items.push_back({"-", "", ""});
            for (int z = 1; z <= max_map; z++)
                n.items.push_back({"Map " + ScaleName(z * map_shrink) + "x", "",
                                   "map zoom " + std::to_string(z)});
            n.items.push_back({"-", "", ""});
            for (int s = 1; s <= max_ui; s++)
                n.items.push_back({"Interface " + std::to_string(s) + "x", "",
                                   "ui scale " + std::to_string(s)});
            if (smooth_ui > 0)
                n.items.push_back(
                        {"Interface " + ScaleName(smooth_ui) + "x smooth", "", "ui scale smooth"});
            n.items.push_back({"-", "", ""});
            n.items.push_back({"Full screen", "", "screen full"});
            n.items.push_back({"Window", "", "screen window"});
            n.items.push_back({"-", "", ""});
            n.items.push_back({"4:3 (original)", "", "screen 4:3"});
            n.items.push_back({"Fit available space", "", "screen fit"});
            n.items.push_back({"-", "", ""});
            n.items.push_back({"CRT screen", "", "screen crt"});
        } else {
            n.items = m.items;
        }
        out.push_back(n);
    }
    return out;
}

// The settings the menus tick when in use (not the original's): their tick
// is a diamond 5 pixels wide in the key column.
static const int kTickW = 5;
static bool Tickable(const std::string& act) {
    for (const char* p : {"map zoom ", "ui scale ", "screen ", "music ", "ai "})
        if (act.rfind(p, 0) == 0) return true;
    return false;
}

Layout MakeLayout(const Font& font, int screen_width, const std::vector<Menu>& menus) {
    Layout out;
    int x = kBarX;
    int lh = font.line_height;
    for (const Menu& m : menus) {
        int tw = (font.Width(m.title) + 7) / 8 * 8;
        int w = 0, key_col = 0, key_w = 0;
        bool ticked = false;
        for (const Item& it : m.items) {
            if (it.label != "-") {
                int lw = 12 + font.Width(it.label);
                if (!it.key.empty()) {
                    key_col = std::max(key_col, lw + 5);
                    key_w = std::max(key_w, font.Width(it.key));
                    lw = key_col + key_w;
                } else if (Tickable(it.act)) {
                    // a setting's tick goes in the key column: keep it clear of the label
                    key_col = std::max(key_col, lw + 5);
                    ticked = true;
                }
                w = std::max(w, lw + 5);
            }
        }
        if (ticked) w = std::max(w, key_col + std::max(key_w, kTickW) + 5);
        int y = kBarH + 1;
        Title t;
        t.title = m.title;
        t.x = x;
        t.w = tw + 8;
        int at = 2;
        for (const Item& it : m.items) {
            int h = it.label == "-" ? 2 : lh + 2;
            t.drop.rows.push_back(Row{it.label, it.key, it.act, x, y + at, w, h});
            at += h;
        }
        int drop_x = std::min(x, screen_width - w);
        for (Row& r : t.drop.rows) r.x = drop_x;
        t.drop.x = drop_x;
        t.drop.y = y;
        t.drop.w = w;
        t.drop.h = at + 1;
        t.drop.key_col = key_col;
        out.push_back(t);
        x += tw + 16;
    }
    return out;
}

int TitleAt(const Layout& lay, int x, int y) {
    if (y < 0 || y >= kBarH) return -1;
    for (size_t i = 0; i < lay.size(); i++) {
        if (x >= lay[i].x && x < lay[i].x + lay[i].w) return static_cast<int>(i);
    }
    return -1;
}

const Row* RowAt(const Layout& lay, int index, int x, int y) {
    if (index < 0 || index >= static_cast<int>(lay.size())) return nullptr;
    for (const Row& r : lay[index].drop.rows) {
        if (r.label != "-" && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return &r;
    }
    return nullptr;
}

}  // namespace menu
