/**
 * @file
 * @brief Report > Diplomacy: the Diplomatic Report and Diplomatic Action, in
 * the 1993 look and the modern one, plus the modern UI's Parley & Attitude
 * Ledger, turn-start envoy missives and the F10 AI Brain Inspector.
 */
#include <algorithm>
#include <cmath>
#include <sstream>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"

namespace diplomacyui {

namespace dip = w2::diplomacy;

namespace {
const kit::Rect kPopupRect{80, 60, 480, 350};  // popup 11
const int kReportDialog = 21, kDone = 368, kAction = 369;
const int kActionDialog = 22, kOk = 370, kBack = 371;
const int kOffer[3] = {372, 380, 388};  // a row of 7 each: war, uneasy, peace

/** @brief Whether side `side` is a valid index, in the scenario and still alive. */
bool Playing(const w2::Game& game, int side) {
    return side >= 0 && side < 8 && game.map->sides[side].in_use && game.map->sides[side].alive;
}

/**
 * @brief The side whose point of view the dialogs show: the player, or else
 * the first side still playing.
 */
int ActiveViewerIndex(const w2::Game& game) {
    if (g_app.player && g_app.player->index >= 0 && g_app.player->index < 8)
        return g_app.player->index;
    for (int i = 0; i < 8; i++) {
        if (Playing(game, i)) return i;
    }
    return 0;
}

/** @brief Draws a piece of DIPLOM.PCK: (source_x, source_y, width, height) at (x, y). */
void Diplom(int source_x, int source_y, int width, int height, int x, int y) {
    if (auto art = g_app.screen->ArtFor(47)) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(art->image, gfx::NewQuad(source_x, source_y, width, height), x, y);
    }
}

/** @brief Draws a side's 16x16 shield. */
void SmallShield(int side, int x, int y) {
    gfx::SetColor(1, 1, 1);
    gfx::Draw(g_app.shields_img, gfx::NewQuad(side * 40 + 24, 46, 16, 16), x, y);
}

/** @brief Draws an empty 40x40 box where a stance would go. */
void Blank(int x, int y) {
    kit::SetPal(0);
    kit::Outline(x, y, 40, 40);
    kit::SetPal(2);
    gfx::Rectangle(gfx::kFill, x + 1, y + 1, 38, 38);
}

// ---------------------------------------------------------------------------
// 1993 Classic Diplomacy UI (default unless --modern-ui is enabled)
// ---------------------------------------------------------------------------

void OpenClassicAction();

/**
 * @brief The classic Diplomatic Report: every pair of sides' stance in a
 * grid, and the sides ranked by their diplomatic rating.
 */
struct ClassicReport : kit::Modal {
    screen::View v;                     ///< Dialog 21 and its controls' states.
    std::map<int, std::string> titles;  ///< Each side's diplomatic title, by side.
    std::vector<w2::Side*> order;       ///< The living sides, by rating.

    /** @brief Draws the grid of stances and the ranking. */
    void Draw() override {
        w2::Game& game = *g_app.g;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6b, 0), 320, 64);
        kit::SetPal(1);
        gfx::Rectangle(gfx::kFill, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 120 + 32 * i, 110, 1, 261);
        gfx::Rectangle(gfx::kFill, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(game, i)) {
                SmallShield(i, 128 + 32 * i, 117);
                SmallShield(i, 96, 146 + 29 * i);
            }
        }
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    Diplom(384, 29, 32, 29, x, y);
                else if (Playing(game, col) && Playing(game, row)) {
                    int state = dip::State(game, col, row);
                    if (state == dip::kWar)
                        Diplom(384, 0, 32, 29, x, y);
                    else if (state == dip::kPeace)
                        Diplom(320, 0, 32, 29, x, y);
                }
            }
        }
        kit::SetPal(0);
        kit::Outline(392, 110, 160, 262);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font, kit::Text(0x6d, 0), 472, 116);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(order[i]->index, 400, y);
            gfx::SetColor(1, 1, 1);
            font.Draw(titles[order[i]->index], 432, y);
        }
        kit::DrawControls(v, hidden);
    }

    /** @brief Done closes the report; Action goes on to Diplomatic Action. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y, hidden);
        if (!control) return;
        if (control->id == kDone)
            kit::Pop(this);
        else if (control->id == kAction) {
            kit::Pop(this);
            OpenClassicAction();
        }
    }

    /** @brief Return and Escape close the report. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

/**
 * @brief Opens the classic Diplomatic Report; Action is hidden when no human
 * is playing.
 */
void OpenClassicReport() {
    auto report = std::make_shared<ClassicReport>();
    report->v = kit::View(kReportDialog);
    report->view = &report->v;
    report->v.state[kDone] = report->v.state[kAction] = w2::uidata::kNormal;
    if (!w2::game::HasActiveHuman(*g_app.g)) report->hidden.insert(kAction);
    report->titles = dip::Ratings(*g_app.g);
    for (w2::Side* side : g_app.g->sides)
        if (side->alive) report->order.push_back(side);
    w2::LuaSort(report->order, [](w2::Side* lhs, w2::Side* rhs) {
        if (lhs->diplo_score != rhs->diplo_score) return lhs->diplo_score < rhs->diplo_score;
        return lhs->index < rhs->index;
    });
    kit::Push(report);
}

/**
 * @brief The classic Diplomatic Action: for each other side its stance, what
 * it proposes, and the three stances the player can propose to it.
 */
struct ClassicAction : kit::Modal {
    screen::View v;           ///< Dialog 22 and its controls' states.
    std::vector<int> others;  ///< The other seven sides, one column each.

    /** @brief Draws the player's side and a column per other side. */
    void Draw() override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        const w2::Side& my_side = game.map->sides[viewer];
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6c, 0), 320, 64);
        kit::Shield(viewer, 96, 111);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        font.Draw(my_side.name, 144, 122);
        const int row_ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) font.Draw(kit::Text(0x6e, i), 104, row_ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::Shield(other, x, 111);
            if (!Playing(game, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(x, y);
            } else {
                int state = dip::State(game, other, viewer);
                Diplom(80 + 120 * state, 45, 40, 40, x, 151);
                int theirs = dip::Proposal(game, other, viewer);
                if (theirs == state)
                    Blank(x, 191);
                else
                    Diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                int mine = dip::Proposal(game, viewer, other);
                for (int stance = 0; stance <= 2; stance++)
                    Diplom(120 * stance + (mine == stance ? 40 : 0), 45, 40, 40, x,
                           245 + 40 * stance);
            }
        }
        std::set<int> offer_controls;
        for (int stance = 0; stance <= 2; stance++)
            for (int i = 0; i <= 6; i++) offer_controls.insert(kOffer[stance] + i);
        kit::DrawControls(v, offer_controls);
    }

    /** @brief OK closes, Back returns to the report, an offer proposes that stance. */
    void Mousepressed(int x, int y, int) override {
        auto control = kit::ControlAt(v, x, y);
        if (!control) return;
        if (control->id == kOk) {
            kit::Pop(this);
            return;
        }
        if (control->id == kBack) {
            kit::Pop(this);
            OpenClassicReport();
            return;
        }
        int viewer = ActiveViewerIndex(*g_app.g);
        for (int stance = 0; stance <= 2; stance++) {
            int column = control->id - kOffer[stance];
            if (column >= 0 && column < 7 && column < static_cast<int>(others.size()))
                dip::Propose(*g_app.g, viewer, others[column], stance);
        }
    }

    /** @brief Return and Escape close the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

/** @brief Opens the classic Diplomatic Action, a column for each side but the viewer. */
void OpenClassicAction() {
    auto action = std::make_shared<ClassicAction>();
    action->v = kit::View(kActionDialog);
    action->view = &action->v;
    for (auto& control : action->v.dialog.controls)
        action->v.state[control.id] = w2::uidata::kNormal;
    int viewer = ActiveViewerIndex(*g_app.g);
    for (int i = 0; i < 8; i++)
        if (i != viewer) action->others.push_back(i);
    kit::Push(action);
}

/**
 * @brief Makes text safe for the bitmap fonts.
 *
 * Turns UTF-8 dashes and curly quotes into ASCII and drops anything else
 * outside printable ASCII. In SSG's TEXT.FNT (`small_font`), '&' is the menu
 * checkmark glyph and '`' a solid block, so those are replaced too.
 */
std::string CleanText(const std::string& raw, bool small_font = true) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned char byte = static_cast<unsigned char>(raw[i]);
        // UTF-8 en-dash / em-dash (E2 80 93 / E2 80 94) and curly quotes
        if (byte == 0xe2 && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x80) {
            unsigned char last_byte = static_cast<unsigned char>(raw[i + 2]);
            if (last_byte == 0x93 || last_byte == 0x94) {
                out += '-';
                i += 2;
                continue;
            }
            if (last_byte == 0x98 || last_byte == 0x99) {
                out += '\'';
                i += 2;
                continue;
            }
            if (last_byte == 0x9c || last_byte == 0x9d) {
                out += '"';
                i += 2;
                continue;
            }
        }
        if (byte < 32 || byte >= 127) continue;
        if (small_font && byte == '&') {
            if (!out.empty() && out.back() == ' ' && i + 1 < raw.size() && raw[i + 1] == ' ') {
                out += "and";
            } else {
                out += '+';
            }
            continue;
        }
        if (byte == '`') {
            out += '\'';
            continue;
        }
        out += static_cast<char>(byte);
    }
    return out;
}

/**
 * @brief Cleans text and cuts it to fit `max_width` pixels, ending in ".."
 * when it is cut.
 */
std::string FitText(const Font& font, const std::string& raw, int max_width,
                    bool small_font = true) {
    std::string text = CleanText(raw, small_font);
    if (max_width <= 0) return "";
    if (font.Width(text) <= max_width) return text;
    const std::string ellipsis = "..";
    int ellipsis_width = font.Width(ellipsis);
    if (ellipsis_width >= max_width) return "";
    while (!text.empty() && font.Width(text) + ellipsis_width > max_width) {
        text.pop_back();
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text + ellipsis;
}

/**
 * @brief Cleans text and wraps it at word breaks into lines of at most
 * `max_width` pixels; a word too long for a line is cut to fit.
 */
std::vector<std::string> WrapText(const Font& font, const std::string& raw, int max_width,
                                  bool small_font = true) {
    std::string cleaned = CleanText(raw, small_font);
    std::vector<std::string> lines;
    if (max_width <= 0 || cleaned.empty()) return lines;
    std::istringstream words(cleaned);
    std::string word, line;
    while (words >> word) {
        if (font.Width(word) > max_width) {
            word = FitText(font, word, max_width, false);
        }
        std::string candidate = line.empty() ? word : line + " " + word;
        if (font.Width(candidate) <= max_width || line.empty()) {
            line = candidate;
        } else {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

/**
 * @brief Draws a recessed dark-slate information well, with an optional
 * header strip: `left_header` on the left in gold, `right_header` on the
 * right in palette colour `right_colour`.
 */
void DrawSunkenPanel(int x, int y, int width, int height, const std::string& left_header = "",
                     const std::string& right_header = "", int right_colour = 5) {
    kit::SetPal(4);  // deep charcoal interior (31,31,31)
    gfx::Rectangle(gfx::kFill, x, y, width, height);
    kit::Bevel(x, y, width, height, 0, 1);  // sunken bevel: black top/left, silver bottom/right
    if (!left_header.empty() || !right_header.empty()) {
        kit::SetPal(3);  // subtle slate header band (44,44,44)
        gfx::Rectangle(gfx::kFill, x + 1, y + 1, width - 2, 17);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, x + 1, y + 18, width - 2, 1);
        const Font& small_font = kit::GetFont(0);
        gfx::SetColor(1, 1, 1);
        int available_width = width - 14;
        std::string right_text =
                right_header.empty() ? ""
                                     : FitText(small_font, right_header, available_width / 2 + 36);
        int right_width = right_text.empty() ? 0 : small_font.Width(right_text) + 10;
        if (!left_header.empty()) {
            std::string left_text =
                    FitText(small_font, left_header, std::max(24, available_width - right_width));
            small_font.Colours(7, 0).Draw(left_text, x + 7, y + 3);
        }
        if (!right_text.empty()) {
            kit::Right(small_font.Colours(right_colour, 0), right_text, x + width - 7, y + 3);
        }
    }
}

/**
 * @brief Draws a raised stone button with its label centred; an active
 * button is filled with palette colour `accent_colour`.
 */
void DrawButton(int x, int y, int width, int height, const std::string& label, bool active = false,
                bool disabled = false, int accent_colour = 11) {
    kit::SetPal(0);
    kit::Outline(x, y, width, height);
    kit::SetPal(disabled ? 3 : (active ? accent_colour : 2));
    gfx::Rectangle(gfx::kFill, x + 1, y + 1, width - 2, height - 2);
    kit::Bevel(x + 1, y + 1, width - 2, height - 2, active ? 7 : (disabled ? 2 : 1), 0);

    const Font& small_font = kit::GetFont(0);
    int glyph_colour = disabled ? 2 : (active ? 7 : 15);
    const Font& ink = small_font.Colours(glyph_colour, 0);
    std::string fitted = FitText(small_font, label, width - 8);
    gfx::SetColor(1, 1, 1);
    kit::Centred(ink, fitted, x + width / 2, y + (height - ink.line_height) / 2);
}

/**
 * @brief The side's banner colours for dark slate panels; black and navy
 * sides (the Lich King's) are lifted to readable tones.
 */
const Font& SideBannerFont(const Font& big_font, const w2::Side& side) {
    if (side.colour == 0) return big_font.Colours(15, side.edge == 0 ? 9 : side.edge);
    if (side.colour == 6) return big_font.Colours(5, 0);
    return big_font.Colours(side.colour, side.edge);
}

void OpenReport();
void OpenAction();

/**
 * @brief The modern Diplomatic Report: the classic one plus alliances, and
 * a click on a side opens the Parley.
 */
struct ReportDialog : kit::Modal {
    screen::View v;                     ///< Dialog 21 and its controls' states.
    std::map<int, std::string> titles;  ///< Each side's diplomatic title, by side.
    std::vector<w2::Side*> order;       ///< The living sides, by rating.

    /** @brief Draws the grid of stances (allies marked), the ranking and a hint. */
    void Draw() override {
        w2::Game& game = *g_app.g;
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6b, 0), 320, 64);
        kit::SetPal(1);
        gfx::Rectangle(gfx::kFill, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 120 + 32 * i, 110, 1, 261);
        gfx::Rectangle(gfx::kFill, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(game, i)) {
                SmallShield(i, 128 + 32 * i, 117);
                SmallShield(i, 96, 146 + 29 * i);
            }
        }
        const Font& tiny_font = kit::GetFont(0);
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    Diplom(384, 29, 32, 29, x, y);
                else if (Playing(game, col) && Playing(game, row)) {
                    int state = dip::State(game, col, row);
                    if (state == dip::kWar)
                        Diplom(384, 0, 32, 29, x, y);
                    else if (state == dip::kPeace) {
                        Diplom(320, 0, 32, 29, x, y);
                        if (dip::IsAllied(game, col, row)) {
                            kit::SetPal(10);
                            kit::Outline(x + 1, y + 1, 30, 27);
                            gfx::SetColor(1, 1, 1);
                            tiny_font.Colours(7, 0).Draw("ALLY", x + 3, y + 2);
                        }
                    }
                }
            }
        }
        // Recessed ratings panel on the right
        kit::SetPal(4);
        gfx::Rectangle(gfx::kFill, 392, 110, 160, 262);
        kit::Bevel(392, 110, 160, 262, 0, 1);
        const Font& font = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(font.Colours(7, 0), kit::Text(0x6d, 0), 472, 116);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 394, 138, 156, 1);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(order[i]->index, 400, y);
            gfx::SetColor(1, 1, 1);
            std::string title = FitText(font, titles[order[i]->index], 120, false);
            font.Draw(title, 424, y);
        }
        // Centred between ACTION (96..160) and DONE (480..544)
        gfx::SetColor(1, 1, 1);
        kit::Centred(tiny_font.Colours(7, 0), "Click any shield to Parley  -  F10: AI Brain", 320,
                     384);
        kit::DrawControls(v);
    }

    /**
     * @brief A click on another side's shield or ranking row opens the
     * Parley with it; Done closes, Action goes on to Diplomatic Action.
     */
    void Mousepressed(int x, int y, int) override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        for (int i = 0; i < 8; i++) {
            if (i == viewer || !Playing(game, i)) continue;
            kit::Rect top_shield{120 + 32 * i, 110, 32, 28};
            kit::Rect left_shield{88, 139 + 29 * i, 32, 29};
            if (top_shield.Contains(x, y) || left_shield.Contains(x, y)) {
                kit::Pop(this);
                OpenParley(i, false);
                return;
            }
        }
        for (size_t i = 0; i < order.size(); i++) {
            int side_index = order[i]->index;
            if (side_index == viewer) continue;
            kit::Rect rate_row{394, 142 + 29 * static_cast<int>(i), 156, 26};
            if (rate_row.Contains(x, y)) {
                kit::Pop(this);
                OpenParley(side_index, false);
                return;
            }
        }
        auto control = kit::ControlAt(v, x, y);
        if (!control) return;
        if (control->id == kDone)
            kit::Pop(this);
        else if (control->id == kAction) {
            kit::Pop(this);
            OpenAction();
        }
    }

    /** @brief F10 opens the AI Brain Inspector; Return and Escape close. */
    void Keypressed(const std::string& key) override {
        if (key == "f10") {
            OpenBrainInspector();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

/** @brief Opens the modern Diplomatic Report. */
void OpenReport() {
    auto report = std::make_shared<ReportDialog>();
    report->v = kit::View(kReportDialog);
    report->view = &report->v;
    report->v.state[kDone] = report->v.state[kAction] = w2::uidata::kNormal;
    report->titles = dip::Ratings(*g_app.g);
    for (w2::Side* side : g_app.g->sides)
        if (side->alive) report->order.push_back(side);
    w2::LuaSort(report->order, [](w2::Side* lhs, w2::Side* rhs) {
        if (lhs->diplo_score != rhs->diplo_score) return lhs->diplo_score < rhs->diplo_score;
        return lhs->index < rhs->index;
    });
    kit::Push(report);
}

/**
 * @brief The modern Diplomatic Action: the classic one plus alliances,
 * truces and gold offers; a click on a side's shield opens the Parley.
 */
struct ActionDialog : kit::Modal {
    screen::View v;           ///< Dialog 22 and its controls' states.
    std::vector<int> others;  ///< The other seven sides, one column each.

    /** @brief Draws the player's side and a column per other side. */
    void Draw() override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        const w2::Side& my_side = game.map->sides[viewer];
        kit::Popup(kPopupRect);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6c, 0), 320, 64);
        kit::Shield(viewer, 96, 111);
        const Font& font = kit::GetFont(2);
        const Font& tiny_font = kit::GetFont(0);
        gfx::SetColor(1, 1, 1);
        SideBannerFont(font, my_side).Draw(FitText(font, my_side.name, 122, false), 144, 122);
        const int row_ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) font.Draw(kit::Text(0x6e, i), 104, row_ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::Shield(other, x, 111);
            if (!Playing(game, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(x, y);
            } else {
                int state = dip::State(game, other, viewer);
                Diplom(80 + 120 * state, 45, 40, 40, x, 151);
                if (dip::IsAllied(game, other, viewer)) {
                    kit::SetPal(10);
                    kit::Outline(x + 1, 152, 38, 38);
                    gfx::SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw("ALLY", x + 6, 153);
                }
                int truce_turns = dip::TruceTurns(game, viewer, other);
                if (truce_turns > 0) {
                    gfx::SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw(absl::StrFormat("T:%d", truce_turns), x + 8, 177);
                }

                int theirs = dip::Proposal(game, other, viewer);
                bool their_ally = dip::ProposalAllied(game, other, viewer);
                if (theirs == state && (!their_ally || dip::IsAllied(game, other, viewer)))
                    Blank(x, 191);
                else {
                    Diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                    if (their_ally) {
                        gfx::SetColor(1, 1, 1);
                        tiny_font.Colours(7, 0).Draw("ALLY", x + 6, 193);
                    }
                }

                int mine = dip::Proposal(game, viewer, other);
                bool my_ally = dip::ProposalAllied(game, viewer, other);
                for (int stance = 0; stance <= 2; stance++) {
                    Diplom(120 * stance + (mine == stance ? 40 : 0), 45, 40, 40, x,
                           245 + 40 * stance);
                }
                if (my_ally && mine == dip::kPeace) {
                    kit::SetPal(10);
                    kit::Outline(x + 1, 246, 38, 38);
                    gfx::SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw("ALLY", x + 6, 247);
                }
                int gold_offer = dip::GoldOffer(game, viewer, other);
                if (gold_offer > 0) {
                    gfx::SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw(absl::StrFormat("+%dg", gold_offer), x + 3, 271);
                }
            }
        }
        // Centred between BACK (96..160) and OK (480..544)
        gfx::SetColor(1, 1, 1);
        kit::Centred(tiny_font.Colours(7, 0), "Click a top shield to Parley  -  F10: AI Brain", 320,
                     384);
        std::set<int> offer_controls;
        for (int stance = 0; stance <= 2; stance++)
            for (int i = 0; i <= 6; i++) offer_controls.insert(kOffer[stance] + i);
        kit::DrawControls(v, offer_controls);
    }

    /**
     * @brief A click on a side's shield opens the Parley; OK closes, Back
     * returns to the report. An offer proposes that stance; offering peace
     * where peace is already proposed toggles proposing an alliance.
     */
    void Mousepressed(int x, int y, int) override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            if (!Playing(game, other)) continue;
            kit::Rect shield_rect{272 + 40 * static_cast<int>(col), 108, 40, 43};
            if (shield_rect.Contains(x, y)) {
                kit::Pop(this);
                OpenParley(other, true);
                return;
            }
        }
        auto control = kit::ControlAt(v, x, y);
        if (!control) return;
        if (control->id == kOk) {
            kit::Pop(this);
            return;
        }
        if (control->id == kBack) {
            kit::Pop(this);
            OpenReport();
            return;
        }
        for (int stance = 0; stance <= 2; stance++) {
            int column = control->id - kOffer[stance];
            if (column >= 0 && column < 7 && column < static_cast<int>(others.size())) {
                int other = others[column];
                if (stance == dip::kPeace && dip::Proposal(game, viewer, other) == dip::kPeace) {
                    bool next_ally = !dip::ProposalAllied(game, viewer, other);
                    dip::ProposeAlliance(game, viewer, other, next_ally);
                } else {
                    dip::Propose(game, viewer, other, stance);
                }
            }
        }
    }

    /** @brief F10 opens the AI Brain Inspector; Return and Escape close. */
    void Keypressed(const std::string& key) override {
        if (key == "f10") {
            OpenBrainInspector();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

/** @brief Opens the modern Diplomatic Action, a column for each side but the viewer. */
void OpenAction() {
    auto action = std::make_shared<ActionDialog>();
    action->v = kit::View(kActionDialog);
    action->view = &action->v;
    int viewer = ActiveViewerIndex(*g_app.g);
    for (auto& control : action->v.dialog.controls)
        action->v.state[control.id] = w2::uidata::kNormal;
    for (int i = 0; i < 8; i++)
        if (i != viewer) action->others.push_back(i);
    kit::Push(action);
}

// ------------------------------------------------------------------ Parley & Attitude Ledger
/**
 * @brief The Parley with one rival: its ruler and stance, the ledger of what
 * shapes its attitude, the treaty and tribute controls, and a forecast of
 * what it would take to win it over.
 */
struct ParleyModal : kit::Modal {
    int other = 0;                  ///< The rival parleyed with.
    bool return_to_action = false;  ///< Back returns to Diplomatic Action, else the report.

    const kit::Rect box{60, 44, 520, 384};             ///< The popup.
    const kit::Rect btn_ally{387, 166, 174, 21};       ///< Propose Alliance.
    const kit::Rect btn_peace{387, 189, 174, 21};      ///< Propose Peace.
    const kit::Rect btn_uneasy{387, 212, 174, 21};     ///< Propose Uneasy.
    const kit::Rect btn_war{387, 235, 174, 21};        ///< Declare / Propose War.
    const kit::Rect btn_gold_minus{387, 260, 84, 21};  ///< Offer 50 gold less.
    const kit::Rect btn_gold_plus{477, 260, 84, 21};   ///< Offer 50 gold more.
    const kit::Rect btn_joint_war{387, 283, 174, 21};  ///< Cycle the joint war target.
    const kit::Rect btn_brain{352, 395, 116, 25};      ///< Opens the AI Brain Inspector.
    const kit::Rect btn_back{476, 395, 92, 25};        ///< Back.

    /** @brief The sides still playing other than the viewer, in index order. */
    std::vector<int> RivalList(const w2::Game& game, int viewer) const {
        std::vector<int> out;
        for (int i = 0; i < 8; i++) {
            if (i != viewer && Playing(game, i)) out.push_back(i);
        }
        return out;
    }

    /**
     * @brief Draws the Parley; a rival that is no longer playing gives way to
     * the first one that is.
     */
    void Draw() override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        if (!Playing(game, other) || other == viewer) {
            auto fallback_rivals = RivalList(game, viewer);
            if (!fallback_rivals.empty()) other = fallback_rivals[0];
        }
        const w2::Side& other_side = game.map->sides[other];
        const w2::Side& my_side = game.map->sides[viewer];
        const auto& profile = w2::ai::personality::ForSide(other_side);
        auto attitude = dip::ComputeAttitude(game, other, viewer);

        kit::Popup(box);
        const Font& big_font = kit::GetFont(2);
        const Font& small_font = kit::GetFont(0);

        // Top Title & Clickable Rival Switcher Shields
        gfx::SetColor(1, 1, 1);
        big_font.Colours(7, 0).Draw("DIPLOMATIC PARLEY", 74, 50);

        auto rivals = RivalList(game, viewer);
        int shields_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            int rival = rivals[idx];
            int shield_x = shields_x + static_cast<int>(idx) * 24;
            if (rival == other) {
                kit::SetPal(7);
                gfx::Rectangle(gfx::kFill, shield_x - 2, 49, 20, 20);
                kit::SetPal(0);
                kit::Outline(shield_x - 3, 48, 22, 22);
            }
            SmallShield(rival, shield_x, 51);
        }

        // Ruler Bio & Stance Banner (72, 73, 496 x 64)
        DrawSunkenPanel(72, 73, 496, 64);
        kit::Shield(other, 80, 85);

        std::string archetype_tag = "[" + CleanText(profile.archetype) + "]";
        int archetype_width = small_font.Width(archetype_tag);
        int max_title_width = 432 - archetype_width - 10;
        std::string side_label = CleanText(other_side.name, false);
        int side_width = big_font.Width(side_label);
        gfx::SetColor(1, 1, 1);
        SideBannerFont(big_font, other_side).Draw(side_label, 128, 77);
        if (side_width + 16 < max_title_width) {
            std::string ruler_part =
                    FitText(big_font, " - " + profile.name, max_title_width - side_width, false);
            big_font.Draw(ruler_part, 128 + side_width, 77);
        }
        kit::Right(small_font.Colours(7, 0), archetype_tag, 560, 79);

        gfx::SetColor(1, 1, 1);
        small_font.Colours(5, 0).Draw(FitText(small_font, "\"" + profile.motto + "\"", 432), 128,
                                      97);

        int state = dip::State(game, viewer, other);
        bool allied = dip::IsAllied(game, viewer, other);
        std::string state_name =
                allied ? "ALLIED"
                       : (state == dip::kPeace
                                  ? "PEACE"
                                  : (state == dip::kIntermediate ? "UNEASY" : "AT WAR"));
        int state_colour =
                (allied || state == dip::kPeace) ? 10 : (state == dip::kIntermediate ? 7 : 8);
        int truce_turns = dip::TruceTurns(game, viewer, other);
        int attitude_colour =
                attitude.total_score >= 15 ? 10 : (attitude.total_score <= -15 ? 8 : 7);

        int text_x = 128;
        text_x += small_font.Colours(state_colour, 0).Draw("[" + state_name + "]", text_x, 115);
        std::string mid_meta = absl::StrFormat("  Truce:%d  Trust:%+d  Attitude:", truce_turns,
                                               attitude.trust_score);
        text_x += small_font.Colours(1, 0).Draw(mid_meta, text_x, 115);
        std::string attitude_tail = FitText(
                small_font,
                absl::StrFormat(" %+d (%s)", attitude.total_score, attitude.stance_summary.c_str()),
                std::max(20, 560 - text_x));
        small_font.Colours(attitude_colour, 0).Draw(attitude_tail, text_x, 115);

        // Left Panel: Attitude Ledger (72, 143, 300 x 167)
        int net_colour = attitude.total_score > 0 ? 10 : (attitude.total_score < 0 ? 8 : 7);
        DrawSunkenPanel(72, 143, 300, 167, "ATTITUDE LEDGER",
                        absl::StrFormat("Total: %+d", attitude.total_score), net_colour);

        gfx::SetScissor(74, 163, 296, 145);
        if (attitude.factors.empty()) {
            small_font.Colours(1, 0).Draw("No active grievances or modifiers.", 82, 172);
        } else {
            int factor_y = 166;
            for (const auto& factor : attitude.factors) {
                if (factor_y > 294) break;
                std::string delta_text = absl::StrFormat("%+d", factor.delta);
                int delta_colour = factor.delta > 0 ? 10 : (factor.delta < 0 ? 8 : 1);
                kit::Right(small_font.Colours(delta_colour, 0), delta_text, 110, factor_y);

                auto factor_lines = WrapText(small_font, factor.label, 246);
                int shown_lines = 0;
                for (const auto& line : factor_lines) {
                    if (shown_lines >= 2 || factor_y > 296) break;
                    small_font.Draw(line, 118, factor_y);
                    factor_y += 13;
                    shown_lines++;
                }
                factor_y += 3;
            }
        }
        gfx::SetScissor();

        // Right Panel: Treaty & Tribute Controls (380, 143, 188 x 167)
        DrawSunkenPanel(380, 143, 188, 167, "PROPOSE TREATY");

        int my_proposal = dip::Proposal(game, viewer, other);
        bool my_alliance_proposal = dip::ProposalAllied(game, viewer, other);
        int gold_offer = dip::GoldOffer(game, viewer, other);
        int war_target = dip::WarTarget(game, viewer, other);
        std::string war_target_name =
                (war_target >= 0 && war_target < 8 && Playing(game, war_target))
                        ? game.map->sides[war_target].name
                        : "None";

        DrawButton(btn_ally.x, btn_ally.y, btn_ally.w, btn_ally.h,
                   my_alliance_proposal ? "* Propose Alliance *" : "Propose Alliance",
                   my_alliance_proposal, false, 11);
        DrawButton(btn_peace.x, btn_peace.y, btn_peace.w, btn_peace.h,
                   (!my_alliance_proposal && my_proposal == dip::kPeace) ? "* Propose Peace *"
                                                                         : "Propose Peace",
                   !my_alliance_proposal && my_proposal == dip::kPeace, false, 11);
        DrawButton(btn_uneasy.x, btn_uneasy.y, btn_uneasy.w, btn_uneasy.h,
                   my_proposal == dip::kIntermediate ? "* Propose Uneasy *" : "Propose Uneasy",
                   my_proposal == dip::kIntermediate, false, 13);
        DrawButton(btn_war.x, btn_war.y, btn_war.w, btn_war.h,
                   my_proposal == dip::kWar ? "* Propose War *" : "Declare / Propose War",
                   my_proposal == dip::kWar, false, 9);

        DrawButton(btn_gold_minus.x, btn_gold_minus.y, btn_gold_minus.w, btn_gold_minus.h,
                   "Gift -50g", false, gold_offer <= 0, 13);
        DrawButton(btn_gold_plus.x, btn_gold_plus.y, btn_gold_plus.w, btn_gold_plus.h,
                   absl::StrFormat("Gift +%dg", gold_offer + 50), gold_offer > 0,
                   gold_offer + 50 > my_side.gold, 13);

        DrawButton(btn_joint_war.x, btn_joint_war.y, btn_joint_war.w, btn_joint_war.h,
                   "War Target: " + war_target_name, war_target != w2::kNone, false, 9);

        // Bottom Panel: Diplomatic Forecast (72, 316, 496 x 72)
        DrawSunkenPanel(
                72, 316, 496, 72, "DIPLOMATIC FORECAST",
                absl::StrFormat("Treasury: %d gp   Gift Offer: %d gp", my_side.gold, gold_offer),
                5);

        gfx::SetScissor(74, 336, 492, 50);
        auto wrapped = WrapText(small_font, attitude.what_it_takes, 478);
        int forecast_y = 339;
        for (size_t i = 0; i < wrapped.size() && i < 2; i++) {
            small_font.Draw(wrapped[i], 80, forecast_y);
            forecast_y += 14;
        }
        if (allied && dip::AlliedTargetCity(game, other) != w2::kNone) {
            if (const w2::City* target_city = game.map->City(dip::AlliedTargetCity(game, other))) {
                std::string target_text = FitText(
                        small_font,
                        absl::StrFormat("Allied War Target: %s (%d, %d)", target_city->name.c_str(),
                                        target_city->x, target_city->y),
                        478);
                small_font.Colours(10, 0).Draw(target_text, 80, 369);
            }
        } else if (forecast_y <= 368) {
            int their_proposal = dip::Proposal(game, other, viewer);
            bool their_ally = dip::ProposalAllied(game, other, viewer);
            std::string their_posture =
                    their_ally
                            ? "Alliance"
                            : (their_proposal == dip::kPeace
                                       ? "Peace"
                                       : (their_proposal == dip::kIntermediate ? "Uneasy" : "War"));
            small_font.Colours(1, 0).Draw(
                    FitText(small_font, "Their current posture toward you: " + their_posture, 478),
                    80, 369);
        }
        gfx::SetScissor();

        // Footer Hints & Action Buttons
        small_font.Colours(1, 0).Draw("Left/Right or click top shields to switch", 74, 401);
        DrawButton(btn_brain.x, btn_brain.y, btn_brain.w, btn_brain.h, "AI Brain (F10)");
        DrawButton(btn_back.x, btn_back.y, btn_back.w, btn_back.h, "Back");
    }

    /**
     * @brief Handles a click: a rival's shield switches to it; the treaty,
     * gold and war target buttons change what the viewer proposes.
     */
    void Mousepressed(int x, int y, int) override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        auto rivals = RivalList(game, viewer);
        int shields_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            kit::Rect shield{shields_x + static_cast<int>(idx) * 24 - 2, 49, 20, 20};
            if (shield.Contains(x, y)) {
                other = rivals[idx];
                return;
            }
        }
        if (btn_ally.Contains(x, y)) {
            dip::Propose(game, viewer, other, dip::kPeace);
            dip::ProposeAlliance(game, viewer, other, true);
        } else if (btn_peace.Contains(x, y)) {
            dip::Propose(game, viewer, other, dip::kPeace);
            dip::ProposeAlliance(game, viewer, other, false);
        } else if (btn_uneasy.Contains(x, y)) {
            dip::Propose(game, viewer, other, dip::kIntermediate);
        } else if (btn_war.Contains(x, y)) {
            dip::Propose(game, viewer, other, dip::kWar);
            dip::SetGoldOffer(game, viewer, other, 0);
        } else if (btn_gold_minus.Contains(x, y)) {
            int offer = dip::GoldOffer(game, viewer, other);
            dip::SetGoldOffer(game, viewer, other, std::max(0, offer - 50));
        } else if (btn_gold_plus.Contains(x, y)) {
            int offer = dip::GoldOffer(game, viewer, other);
            if (offer + 50 <= game.map->sides[viewer].gold) {
                dip::SetGoldOffer(game, viewer, other, offer + 50);
            }
        } else if (btn_joint_war.Contains(x, y)) {
            int target = dip::WarTarget(game, viewer, other);
            int next = w2::kNone;
            int start = (target == w2::kNone) ? 0 : target + 1;
            for (int side = start; side < 8; side++) {
                if (side != viewer && side != other && Playing(game, side)) {
                    next = side;
                    break;
                }
            }
            dip::SetWarTarget(game, viewer, other, next);
        } else if (btn_brain.Contains(x, y)) {
            OpenBrainInspector(other);
        } else if (btn_back.Contains(x, y)) {
            bool back_to_action = return_to_action;
            kit::Pop(this);
            if (back_to_action)
                OpenAction();
            else
                OpenReport();
        }
    }

    /**
     * @brief Left and Right switch rivals, F10 opens the AI Brain Inspector,
     * Return and Escape go back.
     */
    void Keypressed(const std::string& key) override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        if (key == "left" || key == "right") {
            auto rivals = RivalList(game, viewer);
            if (!rivals.empty()) {
                auto found = std::find(rivals.begin(), rivals.end(), other);
                int position =
                        (found == rivals.end()) ? 0 : static_cast<int>(found - rivals.begin());
                int count = static_cast<int>(rivals.size());
                other = rivals[(position + (key == "right" ? 1 : count - 1)) % count];
            }
            return;
        }
        if (key == "f10") {
            OpenBrainInspector(other);
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") {
            bool back_to_action = return_to_action;
            kit::Pop(this);
            if (back_to_action)
                OpenAction();
            else
                OpenReport();
        }
    }
};

// ------------------------------------------------------------------ Turn-Start Envoy Missive Modal
/**
 * @brief A missive from another side's envoy at the start of the player's
 * turn: what it proposes or demands, and why. Accept, open the Parley, or
 * decline; then the next missive shows.
 */
struct MissiveModal : kit::Modal {
    w2::DiplomaticMissive m;  ///< The missive shown.
    kit::Done after;          ///< Called once no missive is left.

    const kit::Rect box{88, 72, 464, 316};           ///< The popup.
    const kit::Rect btn_accept{100, 348, 148, 28};   ///< Accept (and pay any tribute).
    const kit::Rect btn_parley{258, 348, 136, 28};   ///< Open the Parley with the sender.
    const kit::Rect btn_decline{404, 348, 136, 28};  ///< Decline.

    /** @brief Closes this missive and shows the next one, if any. */
    void Finish() {
        auto done = after;
        kit::Pop(this);
        PollMissives(done);
    }

    /** @brief Draws the envoy's banner, the message and its context. */
    void Draw() override {
        w2::Game& game = *g_app.g;
        const w2::Side& sender = game.map->sides[std::clamp(m.from_side, 0, 7)];
        kit::Popup(box);

        const Font& big_font = kit::GetFont(2);
        const Font& small_font = kit::GetFont(0);

        // Top Envoy Banner
        DrawSunkenPanel(100, 84, 440, 56);
        kit::Shield(m.from_side, 108, 92);

        gfx::SetColor(1, 1, 1);
        SideBannerFont(big_font, sender).Draw(FitText(big_font, m.headline, 372, false), 156, 90);

        std::string deal_tag = m.propose_alliance
                                       ? "Proposal: Military Alliance (Shared Vision)"
                                       : (m.proposed_state == dip::kPeace
                                                  ? "Proposal: Peace Treaty"
                                                  : (m.proposed_state == dip::kIntermediate
                                                             ? "Notice: Border Warning / Uneasy"
                                                             : "Declaration"));
        if (m.gold_demand > 0)
            deal_tag += absl::StrFormat("  -  Tribute Demand: %d gp", m.gold_demand);
        else if (m.gold_demand < 0)
            deal_tag += absl::StrFormat("  -  Tribute Gift: +%d gp", -m.gold_demand);
        int deal_colour = (m.propose_alliance || m.proposed_state == dip::kPeace)
                                  ? 10
                                  : (m.proposed_state == dip::kIntermediate ? 7 : 8);
        small_font.Colours(deal_colour, 0).Draw(FitText(small_font, deal_tag, 372), 156, 114);

        // Envoy Message Box
        DrawSunkenPanel(100, 146, 440, 108, "ROYAL ENVOY DISPATCH");
        gfx::SetScissor(102, 166, 436, 86);
        auto body_lines = WrapText(big_font, m.body, 420, false);
        int body_y = 170;
        for (const auto& line : body_lines) {
            if (body_y > 236) break;
            big_font.Draw(line, 108, body_y);
            body_y += 19;
        }
        gfx::SetScissor();

        // Diplomatic Context Box
        DrawSunkenPanel(
                100, 260, 440, 78, "DIPLOMATIC CONTEXT",
                absl::StrFormat("Your Treasury: %d gp", g_app.player ? g_app.player->gold : 0), 5);
        gfx::SetScissor(102, 280, 436, 56);
        auto context_lines = WrapText(small_font, m.reason_summary, 420);
        int context_y = 283;
        for (const auto& line : context_lines) {
            if (context_y > 322) break;
            small_font.Draw(line, 108, context_y);
            context_y += 14;
        }
        gfx::SetScissor();

        int my_gold = g_app.player ? g_app.player->gold : 0;
        bool can_afford = (m.gold_demand <= 0 || my_gold >= m.gold_demand);
        DrawButton(btn_accept.x, btn_accept.y, btn_accept.w, btn_accept.h,
                   m.gold_demand > 0 ? absl::StrFormat("Pay %dg + Accept", m.gold_demand)
                                     : "Accept Proposal",
                   can_afford, !can_afford, 11);
        DrawButton(btn_parley.x, btn_parley.y, btn_parley.w, btn_parley.h, "Open Parley");
        DrawButton(btn_decline.x, btn_decline.y, btn_decline.w, btn_decline.h, "Decline", false,
                   false, 9);
    }

    /**
     * @brief Accept pays any tribute and makes the proposed stance mutual
     * (with a truce and trust for peace or an alliance, and war on any joint
     * target); Open Parley goes to the sender; Decline moves on.
     */
    void Mousepressed(int x, int y, int) override {
        w2::Game& game = *g_app.g;
        int viewer = ActiveViewerIndex(game);
        if (btn_accept.Contains(x, y)) {
            if (m.gold_demand > 0 && game.map->sides[viewer].gold < m.gold_demand) return;
            if (m.gold_demand > 0) {
                game.map->sides[viewer].gold -= m.gold_demand;
                game.map->sides[m.from_side].gold += m.gold_demand;
            }
            dip::Propose(game, viewer, m.from_side, m.proposed_state);
            dip::Propose(game, m.from_side, viewer, m.proposed_state);
            if (m.propose_alliance) {
                dip::SetAllied(game, viewer, m.from_side, true);
                dip::SetTruce(game, viewer, m.from_side, 15);
                dip::AddTrust(game, m.from_side, viewer, 18);
            } else if (m.proposed_state == dip::kPeace) {
                game.diplomacy.state[viewer * 8 + m.from_side] = dip::kPeace;
                game.diplomacy.state[m.from_side * 8 + viewer] = dip::kPeace;
                dip::SetTruce(game, viewer, m.from_side, 10);
                dip::AddTrust(game, m.from_side, viewer, 10);
            }
            if (m.joint_war_target != w2::kNone && Playing(game, m.joint_war_target)) {
                game.diplomacy.state[viewer * 8 + m.joint_war_target] = dip::kWar;
                game.diplomacy.state[m.joint_war_target * 8 + viewer] = dip::kWar;
                dip::Propose(game, viewer, m.joint_war_target, dip::kWar);
            }
            front::StratDirty();
            Finish();
        } else if (btn_parley.Contains(x, y)) {
            int sender = m.from_side;
            kit::Pop(this);
            OpenParley(sender, false);
        } else if (btn_decline.Contains(x, y)) {
            Finish();
        }
    }

    /** @brief Escape declines; Return accepts, as a click on Accept. */
    void Keypressed(const std::string& key) override {
        if (key == "escape")
            Finish();
        else if (key == "return" || key == "kpenter") {
            Mousepressed(btn_accept.x + 5, btn_accept.y + 5, 1);
        }
    }
};

// ------------------------------------------------------------------ F10 AI Brain & Policy
// Inspector
/**
 * @brief The AI Brain Inspector: for one side its personality traits, what
 * it believes about its rivals, and its recent policy decisions; it can also
 * switch the side between the classic AI and the learnable one.
 */
struct BrainInspectorModal : kit::Modal {
    int side_idx = 0;                                     ///< The side inspected.
    const kit::Rect box{44, 34, 552, 412};                ///< The popup.
    const kit::Rect btn_toggle_engine{380, 71, 128, 19};  ///< Classic AI / learnable AI.
    const kit::Rect btn_close{514, 71, 64, 19};           ///< Close.

    /**
     * @brief Draws the inspector; a side that is no longer playing gives way
     * to the first computer still playing, else the viewer.
     */
    void Draw() override {
        w2::Game& game = *g_app.g;
        if (!Playing(game, side_idx)) {
            for (int candidate = 0; candidate < 8; candidate++) {
                if (Playing(game, candidate) && game.map->sides[candidate].computer) {
                    side_idx = candidate;
                    break;
                }
            }
            if (!Playing(game, side_idx)) side_idx = ActiveViewerIndex(game);
        }
        w2::Side& side = game.map->sides[side_idx];
        const auto& profile = w2::ai::personality::ForSide(side);
        const auto& belief = w2::ai::belief::Ensure(side);

        kit::Popup(box);
        const Font& big_font = kit::GetFont(2);
        const Font& small_font = kit::GetFont(0);

        gfx::SetColor(1, 1, 1);
        big_font.Colours(7, 0).Draw("AI BRAIN INSPECTOR (F10)", 56, 42);

        // Top row of clickable faction shields
        for (int i = 0; i < 8; i++) {
            if (!Playing(game, i)) continue;
            int shield_x = 376 + i * 26;
            if (i == side_idx) {
                kit::SetPal(7);
                gfx::Rectangle(gfx::kFill, shield_x - 2, 41, 20, 20);
                kit::SetPal(0);
                kit::Outline(shield_x - 3, 40, 22, 22);
            }
            SmallShield(i, shield_x, 43);
        }

        // Faction Bio & Value Head Summary Banner (56, 67, 528 x 58)
        DrawSunkenPanel(56, 67, 528, 58);
        kit::Shield(side_idx, 64, 76);

        std::string side_title = CleanText(side.name, false);
        int title_width = big_font.Width(side_title);
        gfx::SetColor(1, 1, 1);
        SideBannerFont(big_font, side).Draw(FitText(big_font, side_title, 260, false), 112, 71);
        if (title_width + 16 < 262) {
            big_font.Draw(FitText(big_font, " - " + profile.name, 262 - title_width, false),
                          112 + title_width, 71);
        }
        small_font.Colours(5, 0).Draw(FitText(small_font, "\"" + profile.motto + "\"", 464), 112,
                                      91);

        int explored_percent =
                static_cast<int>(std::round((1.0F - belief.frontier_ratio) * 100.0F));
        std::string evaluation =
                absl::StrFormat("[%s]  V(s):%+.2f  Expl:%d%%  Cit:%d  Gold:%d gp",
                                profile.archetype.c_str(), side.last_board_value, explored_percent,
                                static_cast<int>(belief.visible_cities.size()), side.gold);
        int value_colour =
                side.last_board_value >= 0.05F ? 10 : (side.last_board_value <= -0.05F ? 8 : 7);
        small_font.Colours(value_colour, 0).Draw(FitText(small_font, evaluation, 464), 112, 106);

        DrawButton(btn_toggle_engine.x, btn_toggle_engine.y, btn_toggle_engine.w,
                   btn_toggle_engine.h, side.classic_ai ? "AI: 1993 Classic" : "AI: Learnable RL",
                   !side.classic_ai, false, 11);
        DrawButton(btn_close.x, btn_close.y, btn_close.w, btn_close.h, "Close");

        // Left Panel: 10D Personality Vector Trait Bars (56, 130, 212 x 165)
        DrawSunkenPanel(56, 130, 212, 165, "10D PERSONALITY TRAITS");
        static const char* const kTraitLabels[10] = {
            "Aggression", "Honour",  "Avarice", "Scorched", "HeroFocus",
            "Coalition",  "Cavalry", "Fliers",  "Siege",    "Caution"};
        for (int trait = 0; trait < 10; trait++) {
            int trait_y = 150 + trait * 14;
            small_font.Draw(kTraitLabels[trait], 64, trait_y);
            int bar_x = 160, bar_w = 98, mid_x = bar_x + bar_w / 2;
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, bar_x, trait_y + 3, bar_w, 8);
            kit::Bevel(bar_x, trait_y + 3, bar_w, 8, 0, 2);
            float value = std::clamp(profile.traits[trait], -1.0F, 1.0F);
            int fill_w = static_cast<int>(std::round(std::abs(value) * (bar_w / 2 - 2)));
            kit::SetPal(value >= 0 ? 10 : 8);
            if (fill_w > 0) {
                if (value >= 0)
                    gfx::Rectangle(gfx::kFill, mid_x + 1, trait_y + 4, fill_w, 6);
                else
                    gfx::Rectangle(gfx::kFill, mid_x - fill_w, trait_y + 4, fill_w, 6);
            }
            kit::SetPal(15);
            gfx::Rectangle(gfx::kFill, mid_x, trait_y + 2, 1, 10);
        }

        // Right Panel: Fair-Information Belief Summary (276, 130, 308 x 165)
        DrawSunkenPanel(
                276, 130, 308, 165, "BELIEF MEMORY",
                absl::StrFormat("Fog:%d Stacks:%d", static_cast<int>(belief.fog_frontiers.size()),
                                static_cast<int>(belief.stacks_by_tile.size())),
                5);
        gfx::SetScissor(278, 150, 304, 143);
        small_font.Colours(1, 0).Draw("Rival", 284, 151);
        kit::Right(small_font.Colours(1, 0), "Cit", 434, 151);
        kit::Right(small_font.Colours(1, 0), "Str", 478, 151);
        kit::Right(small_font.Colours(1, 0), "Us / Them", 574, 151);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 282, 165, 296, 1);

        int rival_y = 169;
        for (int rival = 0; rival < 8; rival++) {
            if (rival == side_idx || !Playing(game, rival)) continue;
            if (rival_y > 279) break;
            SmallShield(rival, 284, rival_y - 1);
            std::string rival_name = FitText(small_font, game.map->sides[rival].name, 106);
            small_font.Draw(rival_name, 304, rival_y);
            kit::Right(small_font.Colours(7, 0),
                       absl::StrFormat("%d", belief.visible_cities_by_side[rival]), 434, rival_y);
            kit::Right(small_font.Colours(5, 0),
                       absl::StrFormat("%.1f", belief.seen_enemy_avg_strength[rival]), 478,
                       rival_y);
            std::string border = absl::StrFormat("%.0f / %.0f", belief.my_border_strength[rival],
                                                 belief.their_border_strength[rival]);
            int border_colour =
                    belief.my_border_strength[rival] >= belief.their_border_strength[rival] ? 10
                                                                                            : 8;
            kit::Right(small_font.Colours(border_colour, 0), border, 574, rival_y);
            rival_y += 17;
        }
        gfx::SetScissor();

        // Bottom Panel: Top Scored Neural Policy Decisions (56, 301, 528 x 135)
        DrawSunkenPanel(56, 301, 528, 135, "RECENT NEURAL POLICY DECISIONS",
                        "Left/Right: Switch Side", 1);
        gfx::SetScissor(58, 321, 524, 113);
        if (side.ai_thoughts.empty()) {
            small_font.Colours(1, 0).Draw(
                    "No neural thoughts recorded yet (advance a turn to watch this AI act).", 66,
                    330);
        } else {
            int thought_y = 323;
            int shown = 0;
            // the other thoughts first, then the diplomatic ones
            for (int pass = 0; pass < 2; pass++) {
                for (const auto& thought : side.ai_thoughts) {
                    bool is_diplo = (thought.category == "Diplomacy");
                    if ((pass == 0 && is_diplo) || (pass == 1 && !is_diplo)) continue;
                    if (shown >= 7 || thought_y > 420) break;
                    int category_colour = 15;
                    if (thought.stage_near)
                        category_colour = 7;
                    else if (thought.category == "Economy")
                        category_colour = 5;
                    else if (thought.category == "Spoils")
                        category_colour = 8;
                    else if (thought.category == "Diplomacy")
                        category_colour = 10;

                    std::string short_category =
                            thought.category == "Mission"
                                    ? "Move"
                                    : (thought.category == "Economy"
                                               ? "Econ"
                                               : (thought.category == "Diplomacy"
                                                          ? "Diplo"
                                                          : thought.category));
                    std::string actor_compact = thought.actor_name;
                    size_t unit_pos = actor_compact.find("-unit stack");
                    if (unit_pos != std::string::npos) actor_compact.replace(unit_pos, 11, "u");
                    std::string tag = "[" + short_category + "] ";
                    int tag_width = small_font.Colours(category_colour, 0).Draw(tag, 64, thought_y);
                    std::string rest = FitText(
                            small_font,
                            actor_compact + " -> " + thought.target_name + ": " + thought.detail,
                            512 - tag_width);
                    small_font.Draw(rest, 64 + tag_width, thought_y);
                    thought_y += 15;
                    shown++;
                }
            }
        }
        gfx::SetScissor();
    }

    /** @brief A shield switches side; the engine button toggles the side's AI; Close closes. */
    void Mousepressed(int x, int y, int) override {
        w2::Game& game = *g_app.g;
        for (int i = 0; i < 8; i++) {
            if (!Playing(game, i)) continue;
            kit::Rect shield{374 + i * 26, 41, 20, 20};
            if (shield.Contains(x, y)) {
                side_idx = i;
                return;
            }
        }
        if (btn_toggle_engine.Contains(x, y)) {
            game.map->sides[side_idx].classic_ai = !game.map->sides[side_idx].classic_ai;
            return;
        }
        if (btn_close.Contains(x, y)) {
            kit::Pop(this);
        }
    }

    /** @brief Left and Right switch to the previous or next side playing; F10 and Escape close. */
    void Keypressed(const std::string& key) override {
        w2::Game& game = *g_app.g;
        if (key == "left" || key == "right") {
            int direction = (key == "right") ? 1 : 7;
            for (int step = 1; step <= 8; step++) {
                int candidate = (side_idx + direction * step) % 8;
                if (Playing(game, candidate)) {
                    side_idx = candidate;
                    break;
                }
            }
            return;
        }
        if (key == "f10" || key == "escape" || key == "return" || key == "kpenter") {
            kit::Pop(this);
        }
    }
};

}  // namespace

/** @brief Opens the Diplomatic Report, modern or classic; nothing when diplomacy is off. */
void Open() {
    if (g_app.g->map->options.diplomacy == 0) return;
    if (g_app.modern_ui)
        OpenReport();
    else
        OpenClassicReport();
}

/** @brief Opens Diplomatic Action, modern or classic; nothing when diplomacy is off. */
void Action() {
    if (g_app.g->map->options.diplomacy == 0) return;
    if (g_app.modern_ui)
        OpenAction();
    else
        OpenClassicAction();
}

/** @brief Opens the Parley with a side; the modern UI only. */
void OpenParley(int other_side, bool return_to_action) {
    if (!g_app.modern_ui || !g_app.g || other_side < 0 || other_side >= 8) return;
    auto parley = std::make_shared<ParleyModal>();
    parley->other = other_side;
    parley->return_to_action = return_to_action;
    kit::Push(parley);
}

/**
 * @brief Shows the player's next envoy missive, one after another, then
 * calls `after`.
 *
 * The classic UI has no missives: they are dropped. Missives from sides no
 * longer playing are skipped.
 */
void PollMissives(kit::Done after) {
    if (!g_app.modern_ui) {
        if (g_app.player) g_app.player->missives.clear();
        if (after) after();
        return;
    }
    if (!g_app.g || !g_app.player || g_app.player->computer || g_app.player->missives.empty()) {
        if (after) after();
        return;
    }
    w2::DiplomaticMissive missive = g_app.player->missives.front();
    g_app.player->missives.erase(g_app.player->missives.begin());
    if (!Playing(*g_app.g, missive.from_side)) {
        PollMissives(after);
        return;
    }
    auto dialog = std::make_shared<MissiveModal>();
    dialog->m = std::move(missive);
    dialog->after = after;
    kit::Push(dialog);
}

/**
 * @brief Opens the AI Brain Inspector on a side, or on the first computer
 * playing when that side isn't playing; the modern UI only.
 */
void OpenBrainInspector(int initial_side) {
    if (!g_app.modern_ui || !g_app.g) return;
    auto inspector = std::make_shared<BrainInspectorModal>();
    if (initial_side >= 0 && initial_side < 8 && Playing(*g_app.g, initial_side)) {
        inspector->side_idx = initial_side;
    } else {
        for (int i = 0; i < 8; i++) {
            if (Playing(*g_app.g, i) && g_app.g->map->sides[i].computer) {
                inspector->side_idx = i;
                break;
            }
        }
    }
    kit::Push(inspector);
}

/**
 * @brief Which diplomacy button the side's panel shows: 183 when nothing is
 * pending, 185 when another side proposes friendlier terms, 184 when one
 * proposes more hostile ones (hostile wins); kNone with diplomacy off.
 */
int ButtonFor(w2::Game& game, int side) {
    if (game.map->options.diplomacy == 0) return w2::kNone;
    bool friendlier = false, hostile = false;
    for (int other = 0; other < 8; other++) {
        if (other != side) {
            int state = dip::State(game, other, side), proposal = dip::Proposal(game, other, side);
            if (proposal != state) {
                if (proposal < state)
                    friendlier = true;
                else
                    hostile = true;
            } else if (g_app.modern_ui && dip::ProposalAllied(game, other, side) &&
                       !dip::IsAllied(game, other, side)) {
                friendlier = true;
            }
        }
    }
    if (!(friendlier || hostile)) return 183;
    return hostile ? 184 : 185;
}

}  // namespace diplomacyui
