#include "warlords/uidata.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::uidata {

static const int kMagic = 0x7d00;

Strings LoadStrings(const std::string& path) {
    std::string s = MustRead(path);
    int n_groups = U16(s, 0) / 4;  // the first offset IS the index size
    Strings groups;
    for (int g = 0; g < n_groups; g++) {
        int off = U16(s, static_cast<size_t>(g) * 4), count = U16(s, g * 4 + 2);
        std::vector<std::string> out;
        for (int i = 0; i < count; i++) {
            int at = U16(s, off + i * 2);
            out.push_back(Cstr(s, at, s.size() > static_cast<size_t>(at) ? s.size() - at : 0));
        }
        groups.push_back(out);
    }
    return groups;
}

std::map<int, Join> Joins(const std::string& path) {
    std::string s = MustRead(path);
    std::map<int, Join> out;
    for (size_t i = 0; i < s.size() / 6; i++) {
        size_t at = i * 6;
        out[U16(s, at)] = Join{U16(s, at + 2), U16(s, at + 4)};
    }
    return out;
}

std::map<int, Area> Areas(const std::string& path) {
    std::string s = MustRead(path);
    std::map<int, Area> out;
    size_t at = 0;
    while (at + 13 < s.size()) {
        if (U16(s, at) != kMagic) throw std::runtime_error("AREA.DAT: lost the record boundary");
        Area a;
        a.id = U16(s, at + 2);
        a.enabled = U16(s, at + 4);
        int count = U16(s, at + 6);
        for (int i = 0; i < count; i++) {
            size_t r = at + 14 + static_cast<size_t>(i) * 14;
            a.regions.push_back(
                    Region{U16(s, r), U16(s, r + 4), U16(s, r + 6), U16(s, r + 8), U16(s, r + 10)});
        }
        out[a.id] = a;
        at += 14 + count * 14;
    }
    return out;
}

std::map<int, ButtonGroup> Buttons(const std::string& path) {
    std::string s = MustRead(path);
    std::map<int, ButtonGroup> out;
    size_t at = 0;
    while (at + 32 < s.size()) {
        if (U16(s, at) != kMagic) throw std::runtime_error("BUTTON.DAT: lost the record boundary");
        ButtonGroup b;
        b.id = U16(s, at + 2);
        int count = U16(s, at + 4);
        for (int i = 0; i < count; i++) {
            size_t c = at + 33 + static_cast<size_t>(i) * 33;
            Control ctl;
            for (int st = 0; st <= 2; st++)
                ctl.src[st] = Point{U16(s, c + 19 + static_cast<size_t>(st) * 4),
                                    U16(s, c + 21 + static_cast<size_t>(st) * 4)};
            ctl.id = U16(s, c);
            ctl.x = U16(s, c + 6);
            ctl.y = U16(s, c + 8);
            ctl.w = U8(s, c + 10);
            ctl.h = U8(s, c + 11);
            ctl.bitmap = U16(s, c + 31);
            b.controls.push_back(ctl);
        }
        out[b.id] = b;
        at += 33 + count * 33;
    }
    return out;
}

std::map<int, ShortcutItem> ShortcutItems(const std::string& path) {
    std::string s = MustRead(path);
    std::map<int, ShortcutItem> out;
    for (size_t i = 0; (i + 1) * 68 <= s.size(); i++) {
        size_t at = i * 68;
        int id = U16(s, at + 2);
        std::string name = Cstr(s, at + 4, 50);
        int x = U16(s, at + 54), y = U16(s, at + 56);
        if (!name.empty()) {
            ShortcutItem it;
            it.name = name;
            it.w = U16(s, at + 66);
            it.h = U16(s, at + 60);
            it.src = {Point{x + U16(s, at + 66), y}, Point{x, y},
                      Point{U16(s, at + 62), U16(s, at + 64)}};
            out[id] = it;
        }
    }
    return out;
}

std::map<int, int> Shortcuts(const std::string& path) {
    std::string s = MustRead(path);
    std::map<int, int> out;
    for (size_t i = 0; i < s.size() / 2; i++) out[static_cast<int>(i)] = U16(s, i * 2);
    return out;
}

UI Load(const std::string& data_dir) {
    std::string d = data_dir + "/DATA/";
    UI ui;
    ui.files = LoadStrings(d + "FILE.DAT");
    if (ui.files.size() > 3)
        for (size_t i = 0; i < ui.files[3].size(); i++)
            ui.bitmaps[static_cast<int>(i)] = Lower(ui.files[3][i]);
    try {
        ui.shortcut_items = ShortcutItems(data_dir + "/UDB/UDB.DAT");
        ui.shortcuts = Shortcuts(data_dir + "/UDB/UDB.CUR");
    } catch (const std::exception&) {
        ui.shortcut_items.clear();
        ui.shortcuts.clear();
    }
    for (auto& [id, it] : ui.shortcut_items) ui.shortcut_names[id] = it.name;
    ui.joins = Joins(d + "JOIN.DAT");
    ui.areas = Areas(d + "AREA.DAT");
    ui.buttons = Buttons(d + "BUTTON.DAT");
    ui.text = LoadStrings(d + "STRING.DAT");
    return ui;
}

std::string Text(const UI& ui, int group, int index) {
    if (group < 0 || group >= static_cast<int>(ui.text.size())) return "";
    const auto& g = ui.text[group];
    if (index < 0 || index >= static_cast<int>(g.size())) return "";
    return g[index];
}

Dialog FindDialog(const UI& ui, int id) {
    Dialog d;
    d.id = id;
    auto j = ui.joins.find(id);
    if (j == ui.joins.end()) return d;
    d.found = true;
    auto b = ui.buttons.find(j->second.button);
    auto a = ui.areas.find(j->second.area);
    if (b != ui.buttons.end()) d.controls = b->second.controls;
    if (a != ui.areas.end()) {
        d.regions = a->second.regions;
        d.enabled = a->second.enabled;
    }
    return d;
}

}  // namespace w2::uidata
