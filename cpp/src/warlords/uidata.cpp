#include "warlords/uidata.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::uidata {

/// The word that opens every AREA.DAT and BUTTON.DAT record.
static const int kMagic = 0x7d00;

Strings LoadStrings(const std::string& path) {
    std::string bytes = MustRead(path);
    int n_groups = U16(bytes, 0) / 4;  // the first offset IS the index size
    Strings groups;
    for (int group = 0; group < n_groups; group++) {
        int offset = U16(bytes, static_cast<size_t>(group) * 4), count = U16(bytes, group * 4 + 2);
        std::vector<std::string> out;
        for (int i = 0; i < count; i++) {
            int pos = U16(bytes, offset + i * 2);
            out.push_back(Cstr(bytes, pos,
                               bytes.size() > static_cast<size_t>(pos) ? bytes.size() - pos : 0));
        }
        groups.push_back(out);
    }
    return groups;
}

std::map<int, Join> Joins(const std::string& path) {
    std::string bytes = MustRead(path);
    std::map<int, Join> out;
    for (size_t i = 0; i < bytes.size() / 6; i++) {
        size_t pos = i * 6;
        out[U16(bytes, pos)] = Join{U16(bytes, pos + 2), U16(bytes, pos + 4)};
    }
    return out;
}

std::map<int, Area> Areas(const std::string& path) {
    std::string bytes = MustRead(path);
    std::map<int, Area> out;
    size_t pos = 0;
    while (pos + 13 < bytes.size()) {
        if (U16(bytes, pos) != kMagic)
            throw std::runtime_error("AREA.DAT: lost the record boundary");
        Area area;
        area.id = U16(bytes, pos + 2);
        area.enabled = U16(bytes, pos + 4);
        int count = U16(bytes, pos + 6);
        for (int i = 0; i < count; i++) {
            size_t region = pos + 14 + static_cast<size_t>(i) * 14;
            area.regions.push_back(Region{U16(bytes, region), U16(bytes, region + 4),
                                          U16(bytes, region + 6), U16(bytes, region + 8),
                                          U16(bytes, region + 10)});
        }
        out[area.id] = area;
        pos += 14 + count * 14;
    }
    return out;
}

std::map<int, ButtonGroup> Buttons(const std::string& path) {
    std::string bytes = MustRead(path);
    std::map<int, ButtonGroup> out;
    size_t pos = 0;
    while (pos + 32 < bytes.size()) {
        if (U16(bytes, pos) != kMagic)
            throw std::runtime_error("BUTTON.DAT: lost the record boundary");
        ButtonGroup group;
        group.id = U16(bytes, pos + 2);
        int count = U16(bytes, pos + 4);
        for (int i = 0; i < count; i++) {
            size_t record = pos + 33 + static_cast<size_t>(i) * 33;
            Control control;
            for (int state = 0; state <= 2; state++)
                control.src[state] =
                        Point{U16(bytes, record + 19 + static_cast<size_t>(state) * 4),
                              U16(bytes, record + 21 + static_cast<size_t>(state) * 4)};
            control.id = U16(bytes, record);
            control.x = U16(bytes, record + 6);
            control.y = U16(bytes, record + 8);
            control.w = U8(bytes, record + 10);
            control.h = U8(bytes, record + 11);
            control.bitmap = U16(bytes, record + 31);
            group.controls.push_back(control);
        }
        out[group.id] = group;
        pos += 33 + count * 33;
    }
    return out;
}

std::map<int, ShortcutItem> ShortcutItems(const std::string& path) {
    std::string bytes = MustRead(path);
    std::map<int, ShortcutItem> out;
    for (size_t i = 0; (i + 1) * 68 <= bytes.size(); i++) {
        size_t pos = i * 68;
        int item_id = U16(bytes, pos + 2);
        std::string name = Cstr(bytes, pos + 4, 50);
        int x = U16(bytes, pos + 54), y = U16(bytes, pos + 56);
        if (!name.empty()) {
            ShortcutItem item;
            item.name = name;
            item.w = U16(bytes, pos + 66);
            item.h = U16(bytes, pos + 60);
            item.src = {Point{x + U16(bytes, pos + 66), y}, Point{x, y},
                        Point{U16(bytes, pos + 62), U16(bytes, pos + 64)}};
            out[item_id] = item;
        }
    }
    return out;
}

std::map<int, int> Shortcuts(const std::string& path) {
    std::string bytes = MustRead(path);
    std::map<int, int> out;
    for (size_t i = 0; i < bytes.size() / 2; i++) out[static_cast<int>(i)] = U16(bytes, i * 2);
    return out;
}

UI Load(const std::string& data_dir) {
    std::string dir = data_dir + "/DATA/";
    UI layout;
    layout.files = LoadStrings(dir + "FILE.DAT");
    if (layout.files.size() > 3)
        for (size_t i = 0; i < layout.files[3].size(); i++)
            layout.bitmaps[static_cast<int>(i)] = Lower(layout.files[3][i]);
    try {
        layout.shortcut_items = ShortcutItems(data_dir + "/UDB/UDB.DAT");
        layout.shortcuts = Shortcuts(data_dir + "/UDB/UDB.CUR");
    } catch (const std::exception&) {
        layout.shortcut_items.clear();
        layout.shortcuts.clear();
    }
    for (auto& [item_id, item] : layout.shortcut_items) layout.shortcut_names[item_id] = item.name;
    layout.joins = Joins(dir + "JOIN.DAT");
    layout.areas = Areas(dir + "AREA.DAT");
    layout.buttons = Buttons(dir + "BUTTON.DAT");
    layout.text = LoadStrings(dir + "STRING.DAT");
    return layout;
}

std::string Text(const UI& layout, int group, int index) {
    if (group < 0 || group >= static_cast<int>(layout.text.size())) return "";
    const auto& strings = layout.text[group];
    if (index < 0 || index >= static_cast<int>(strings.size())) return "";
    return strings[index];
}

Dialog FindDialog(const UI& layout, int dialog_id) {
    Dialog dialog;
    dialog.id = dialog_id;
    auto join = layout.joins.find(dialog_id);
    if (join == layout.joins.end()) return dialog;
    dialog.found = true;
    auto buttons = layout.buttons.find(join->second.button);
    auto area = layout.areas.find(join->second.area);
    if (buttons != layout.buttons.end()) dialog.controls = buttons->second.controls;
    if (area != layout.areas.end()) {
        dialog.regions = area->second.regions;
        dialog.enabled = area->second.enabled;
    }
    return dialog;
}

}  // namespace w2::uidata
