#include "warlords/uidata.hpp"

#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::uidata {

/// The word that opens every AREA.DAT and BUTTON.DAT record.
constexpr int kMagic = 0x7d00;

absl::StatusOr<Strings> LoadStrings(const std::filesystem::path& path) {
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    int n_groups = U16(bytes, 0) / 4;  // the first offset IS the index size
    Strings groups;
    for (int group = 0; group < n_groups; group++) {
        size_t header = static_cast<size_t>(group) * 4;
        int offset = U16(bytes, header);
        int count = U16(bytes, header + 2);
        std::vector<std::string> out;
        for (int i = 0; i < count; i++) {
            size_t pos = static_cast<size_t>(
                    U16(bytes, static_cast<size_t>(offset) + static_cast<size_t>(i) * 2));
            out.push_back(Cstr(bytes, pos, bytes.size() > pos ? bytes.size() - pos : 0));
        }
        groups.push_back(std::move(out));
    }
    return groups;
}

absl::StatusOr<std::map<int, Join>> Joins(const std::filesystem::path& path) {
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    std::map<int, Join> out;
    for (size_t i = 0; i < bytes.size() / 6; i++) {
        size_t pos = i * 6;
        out[U16(bytes, pos)] = Join{U16(bytes, pos + 2), U16(bytes, pos + 4)};
    }
    return out;
}

absl::StatusOr<std::map<int, Area>> Areas(const std::filesystem::path& path) {
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    std::map<int, Area> out;
    size_t pos = 0;
    while (pos + 13 < bytes.size()) {
        int magic = U16(bytes, pos);
        if (magic != kMagic) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to parse UI area table '%s': expected record magic 0x%04x at byte "
                    "offset %zu, got 0x%04x; verify DATA/AREA.DAT is intact.",
                    path.string(), kMagic, pos, magic));
        }
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
        out[area.id] = std::move(area);
        pos += 14 + static_cast<size_t>(count) * 14;
    }
    return out;
}

absl::StatusOr<std::map<int, ButtonGroup>> Buttons(const std::filesystem::path& path) {
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    std::map<int, ButtonGroup> out;
    size_t pos = 0;
    while (pos + 32 < bytes.size()) {
        int magic = U16(bytes, pos);
        if (magic != kMagic) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to parse UI button table '%s': expected record magic 0x%04x at byte "
                    "offset %zu, got 0x%04x; verify DATA/BUTTON.DAT is intact.",
                    path.string(), kMagic, pos, magic));
        }
        ButtonGroup group;
        group.id = U16(bytes, pos + 2);
        int count = U16(bytes, pos + 4);
        for (int i = 0; i < count; i++) {
            size_t record = pos + 33 + static_cast<size_t>(i) * 33;
            Control control;
            for (int state = 0; state <= 2; state++) {
                control.src[state] =
                        Point{U16(bytes, record + 19 + static_cast<size_t>(state) * 4),
                              U16(bytes, record + 21 + static_cast<size_t>(state) * 4)};
            }
            control.id = U16(bytes, record);
            control.x = U16(bytes, record + 6);
            control.y = U16(bytes, record + 8);
            control.w = U8(bytes, record + 10);
            control.h = U8(bytes, record + 11);
            control.bitmap = U16(bytes, record + 31);
            group.controls.push_back(control);
        }
        out[group.id] = std::move(group);
        pos += 33 + static_cast<size_t>(count) * 33;
    }
    return out;
}

absl::StatusOr<std::map<int, ShortcutItem>> ShortcutItems(const std::filesystem::path& path) {
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    std::map<int, ShortcutItem> out;
    for (size_t i = 0; (i + 1) * 68 <= bytes.size(); i++) {
        size_t pos = i * 68;
        int item_id = U16(bytes, pos + 2);
        std::string name = Cstr(bytes, pos + 4, 50);
        int x = U16(bytes, pos + 54);
        int y = U16(bytes, pos + 56);
        if (!name.empty()) {
            ShortcutItem item;
            item.name = name;
            item.w = U16(bytes, pos + 66);
            item.h = U16(bytes, pos + 60);
            item.src = {Point{x + U16(bytes, pos + 66), y}, Point{x, y},
                        Point{U16(bytes, pos + 62), U16(bytes, pos + 64)}};
            out[item_id] = std::move(item);
        }
    }
    return out;
}

absl::StatusOr<std::map<int, int>> Shortcuts(const std::filesystem::path& path) {
    auto file_bytes = ReadBytes(path);
    if (!file_bytes.ok()) return file_bytes.status();
    absl::Span<const uint8_t> bytes(*file_bytes);
    std::map<int, int> out;
    for (size_t i = 0; i < bytes.size() / 2; i++) {
        out[static_cast<int>(i)] = U16(bytes, i * 2);
    }
    return out;
}

absl::StatusOr<UI> Load(const std::filesystem::path& data_dir) {
    std::filesystem::path dir = data_dir / "DATA";
    UI layout;
    auto files = LoadStrings(dir / "FILE.DAT");
    if (!files.ok()) return files.status();
    layout.files = *std::move(files);
    if (layout.files.size() > 3) {
        for (size_t i = 0; i < layout.files[3].size(); i++) {
            layout.bitmaps[static_cast<int>(i)] = Lower(layout.files[3][i]);
        }
    }
    auto shortcut_items = ShortcutItems(data_dir / "UDB/UDB.DAT");
    auto shortcuts = Shortcuts(data_dir / "UDB/UDB.CUR");
    if (shortcut_items.ok() && shortcuts.ok()) {
        layout.shortcut_items = *std::move(shortcut_items);
        layout.shortcuts = *std::move(shortcuts);
    }
    for (const auto& [item_id, item] : layout.shortcut_items) {
        layout.shortcut_names[item_id] = item.name;
    }
    auto joins = Joins(dir / "JOIN.DAT");
    if (!joins.ok()) return joins.status();
    layout.joins = *std::move(joins);

    auto areas = Areas(dir / "AREA.DAT");
    if (!areas.ok()) return areas.status();
    layout.areas = *std::move(areas);

    auto buttons = Buttons(dir / "BUTTON.DAT");
    if (!buttons.ok()) return buttons.status();
    layout.buttons = *std::move(buttons);

    auto text = LoadStrings(dir / "STRING.DAT");
    if (!text.ok()) return text.status();
    layout.text = *std::move(text);
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
