/**
 * @file
 * @brief Report > Diplomacy: the Diplomatic Report and Diplomatic Action, in
 * the 1993 look and the modern one, plus the modern UI's Parley & Attitude
 * Ledger, turn-start envoy missives and the F10 AI Brain Inspector.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_format.h"

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"

namespace diplomacyui {

namespace dip = w2::diplomacy;

namespace {

using TitleMap = std::map<int, std::string>;
using SideList = std::vector<w2::Side*>;
using SideIndexList = std::vector<int>;
using LineList = std::vector<std::string>;

constexpr kit::Rect kPopupRect{80, 60, 480, 350};  // popup 11
constexpr int kReportDialog = 21;
constexpr int kDone = 368;
constexpr int kAction = 369;
constexpr int kActionDialog = 22;
constexpr int kOk = 370;
constexpr int kBack = 371;
constexpr std::array<int, 3> kOffer = {372, 380, 388};  // a row of 7 each: war, uneasy, peace
constexpr std::array<int, 7> kActionRowYs = {165, 205, 251, 271, 291, 311, 331};
constexpr std::array<const char*, 10> kTraitLabels = {
    "Aggression", "Honour",  "Avarice", "Scorched", "HeroFocus",
    "Coalition",  "Cavalry", "Fliers",  "Siege",    "Caution"};

/** @brief Whether side `side` is a valid index, in the scenario and still alive. */
bool Playing(const w2::Game& game, int side) {
    return side >= 0 && side < 8 && game.GetMap().Sides()[side].IsActive();
}

/**
 * @brief The side whose point of view the dialogs show: the player, or else
 * the first side still playing.
 */
int ActiveViewerIndex(const w2::UiContext& ctx, const w2::Game& game) {
    if (ctx.Player() && ctx.Player()->Index() >= 0 && ctx.Player()->Index() < 8) {
        return ctx.Player()->Index();
    }
    for (int i = 0; i < 8; i++) {
        if (Playing(game, i)) return i;
    }
    return 0;
}

/** @brief Draws a piece of `DIPLOM.PCK`: `(source_x, source_y, width, height)` at `(x, y)`. */
void Diplom(w2::UiContext& ctx, int source_x, int source_y, int width, int height, int x, int y) {
    if (const auto* art = ctx.ControlArt(47)) {
        ctx.Renderer().SetColor(1, 1, 1);
        ctx.Renderer().Draw(art->image, gfx::NewQuad(source_x, source_y, width, height), x, y);
    }
}

/** @brief Draws a side's 16x16 shield. */
void SmallShield(w2::UiContext& ctx, int side, int x, int y) {
    ctx.Renderer().SetColor(1, 1, 1);
    ctx.Renderer().Draw(ctx.Assets().ShieldsImg(), gfx::NewQuad(side * 40 + 24, 46, 16, 16), x, y);
}

/** @brief Draws an empty 40x40 box where a stance would go. */
void Blank(w2::UiContext& ctx, int x, int y) {
    kit::SetPal(ctx, 0);
    kit::Outline(ctx, x, y, 40, 40);
    kit::SetPal(ctx, 2);
    ctx.Renderer().Rectangle(gfx::kFill, x + 1, y + 1, 38, 38);
}

// ---------------------------------------------------------------------------
// 1993 Classic Diplomacy UI (default unless --modern-ui is enabled)
// ---------------------------------------------------------------------------

void OpenClassicReport(w2::UiContext& ctx);
void OpenClassicAction(w2::UiContext& ctx);

/**
 * @brief The classic Diplomatic Report: every pair of sides' stance in a
 * grid, and the sides ranked by their diplomatic rating.
 */
class ClassicReport final : public kit::Modal {
  public:
    explicit ClassicReport(w2::UiContext& ctx)
            : kit::Modal(ctx), view_(kit::View(ctx, kReportDialog)) {
        SetDialogView(&view_);
        view_.state[kDone] = w2::uidata::kNormal;
        view_.state[kAction] = w2::uidata::kNormal;
        w2::Game& game = ctx.ActiveGame();
        if (!w2::game::HasActiveHuman(game)) HiddenControls().insert(kAction);
        titles_ = dip::Ratings(game);
        for (w2::Side* side : game.Sides()) {
            if (side->IsAlive()) order_.push_back(side);
        }
        w2::LuaSort(order_, [](w2::Side* lhs, w2::Side* rhs) {
            if (lhs->DiploScore() != rhs->DiploScore()) {
                return lhs->DiploScore() < rhs->DiploScore();
            }
            return lhs->Index() < rhs->Index();
        });
    }

    /** @brief Draws the grid of stances and the ranking. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0x6b, 0), 320, 64);
        kit::SetPal(ctx, 1);
        ren.Rectangle(gfx::kFill, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) ren.Rectangle(gfx::kFill, 120 + 32 * i, 110, 1, 261);
        ren.Rectangle(gfx::kFill, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) ren.Rectangle(gfx::kFill, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(game, i)) {
                SmallShield(ctx, i, 128 + 32 * i, 117);
                SmallShield(ctx, i, 96, 146 + 29 * i);
            }
        }
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col;
                int y = 139 + 29 * row;
                if (col == row) {
                    Diplom(ctx, 384, 29, 32, 29, x, y);
                } else if (Playing(game, col) && Playing(game, row)) {
                    int state = dip::State(game, col, row);
                    if (state == dip::kWar) {
                        Diplom(ctx, 384, 0, 32, 29, x, y);
                    } else if (state == dip::kPeace) {
                        Diplom(ctx, 320, 0, 32, 29, x, y);
                    }
                }
            }
        }
        kit::SetPal(ctx, 0);
        kit::Outline(ctx, 392, 110, 160, 262);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(font, kit::Text(ctx, 0x6d, 0), 472, 116);
        for (size_t i = 0; i < order_.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(ctx, order_[i]->Index(), 400, y);
            ren.SetColor(1, 1, 1);
            font.Draw(titles_[order_[i]->Index()], 432, y);
        }
        kit::DrawControls(ctx, view_, HiddenControls());
    }

    /** @brief Done closes the report; Action goes on to Diplomatic Action. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(view_, x, y, HiddenControls());
        if (!control) return;
        if (control->id == kDone) {
            PopSelf();
        } else if (control->id == kAction) {
            PopSelf();
            OpenClassicAction(ctx);
        }
    }

    /** @brief Return and Escape close the report. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View view_;  ///< Dialog 21 and its controls' states.
    TitleMap titles_;    ///< Each side's diplomatic title, by side.
    SideList order_;     ///< The living sides, by rating.
};

/**
 * @brief Opens the classic Diplomatic Report; Action is hidden when no human
 * is playing.
 */
void OpenClassicReport(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<ClassicReport>(ctx));
}

/**
 * @brief The classic Diplomatic Action: for each other side its stance, what
 * it proposes, and the three stances the player can propose to it.
 */
class ClassicAction final : public kit::Modal {
  public:
    explicit ClassicAction(w2::UiContext& ctx)
            : kit::Modal(ctx), view_(kit::View(ctx, kActionDialog)) {
        SetDialogView(&view_);
        for (const auto& control : view_.dialog.controls) {
            view_.state[control.id] = w2::uidata::kNormal;
        }
        int viewer = ActiveViewerIndex(ctx, ctx.ActiveGame());
        for (int i = 0; i < 8; i++) {
            if (i != viewer) others_.push_back(i);
        }
    }

    /** @brief Draws the player's side and a column per other side. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        const w2::Side& my_side = game.GetMap().Sides()[viewer];
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0x6c, 0), 320, 64);
        kit::Shield(ctx, viewer, 96, 111);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        font.Draw(my_side.Name(), 144, 122);
        for (int i = 0; i < 7; i++) font.Draw(kit::Text(ctx, 0x6e, i), 104, kActionRowYs[i]);
        for (size_t col = 0; col < others_.size(); col++) {
            int other = others_[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::Shield(ctx, other, x, 111);
            if (!Playing(game, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(ctx, x, y);
            } else {
                int state = dip::State(game, other, viewer);
                Diplom(ctx, 80 + 120 * state, 45, 40, 40, x, 151);
                int theirs = dip::Proposal(game, other, viewer);
                if (theirs == state) {
                    Blank(ctx, x, 191);
                } else {
                    Diplom(ctx, 80 + 120 * theirs, 45, 40, 40, x, 191);
                }
                int mine = dip::Proposal(game, viewer, other);
                for (int stance = 0; stance <= 2; stance++) {
                    Diplom(ctx, 120 * stance + (mine == stance ? 40 : 0), 45, 40, 40, x,
                           245 + 40 * stance);
                }
            }
        }
        kit::ControlIdSet offer_controls;
        for (int stance = 0; stance <= 2; stance++) {
            for (int i = 0; i <= 6; i++) offer_controls.insert(kOffer[stance] + i);
        }
        kit::DrawControls(ctx, view_, offer_controls);
    }

    /** @brief OK closes, Back returns to the report, an offer proposes that stance. */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        const auto* control = kit::ControlAt(view_, x, y);
        if (!control) return;
        if (control->id == kOk) {
            PopSelf();
            return;
        }
        if (control->id == kBack) {
            PopSelf();
            OpenClassicReport(ctx);
            return;
        }
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        for (int stance = 0; stance <= 2; stance++) {
            int column = control->id - kOffer[stance];
            if (column >= 0 && column < 7 && column < static_cast<int>(others_.size())) {
                dip::Propose(game, viewer, others_[column], stance);
            }
        }
    }

    /** @brief Return and Escape close the dialog. */
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View view_;     ///< Dialog 22 and its controls' states.
    SideIndexList others_;  ///< The other seven sides, one column each.
};

/** @brief Opens the classic Diplomatic Action, a column for each side but the viewer. */
void OpenClassicAction(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<ClassicAction>(ctx));
}

/**
 * @brief Makes text safe for the bitmap fonts.
 *
 * Turns UTF-8 dashes and curly quotes into ASCII and drops anything else
 * outside printable ASCII. In SSG's `TEXT.FNT` (`small_font`), `'&'` is the menu
 * checkmark glyph and `` '`' `` a solid block, so those are replaced too.
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
 * @brief Cleans text and cuts it to fit `max_width` pixels, ending in `".."`
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
LineList WrapText(const Font& font, const std::string& raw, int max_width, bool small_font = true) {
    std::string cleaned = CleanText(raw, small_font);
    LineList lines;
    if (max_width <= 0 || cleaned.empty()) return lines;
    std::istringstream words(cleaned);
    std::string word;
    std::string line;
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
void DrawSunkenPanel(w2::UiContext& ctx, int x, int y, int width, int height,
                     const std::string& left_header = "", const std::string& right_header = "",
                     int right_colour = 5) {
    gfx::Renderer& ren = ctx.Renderer();
    kit::SetPal(ctx, 4);  // deep charcoal interior (31,31,31)
    ren.Rectangle(gfx::kFill, x, y, width, height);
    kit::Bevel(ctx, x, y, width, height, 0, 1);  // sunken bevel: black top/left, silver bot/right
    if (!left_header.empty() || !right_header.empty()) {
        kit::SetPal(ctx, 3);  // subtle slate header band (44,44,44)
        ren.Rectangle(gfx::kFill, x + 1, y + 1, width - 2, 17);
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kFill, x + 1, y + 18, width - 2, 1);
        const Font& small_font = kit::GetFont(ctx, 0);
        ren.SetColor(1, 1, 1);
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
void DrawButton(w2::UiContext& ctx, int x, int y, int width, int height, const std::string& label,
                bool active = false, bool disabled = false, int accent_colour = 11) {
    gfx::Renderer& ren = ctx.Renderer();
    kit::SetPal(ctx, 0);
    kit::Outline(ctx, x, y, width, height);
    kit::SetPal(ctx, disabled ? 3 : (active ? accent_colour : 2));
    ren.Rectangle(gfx::kFill, x + 1, y + 1, width - 2, height - 2);
    kit::Bevel(ctx, x + 1, y + 1, width - 2, height - 2, active ? 7 : (disabled ? 2 : 1), 0);

    const Font& small_font = kit::GetFont(ctx, 0);
    int glyph_colour = disabled ? 2 : (active ? 7 : 15);
    const Font& ink = small_font.Colours(glyph_colour, 0);
    std::string fitted = FitText(small_font, label, width - 8);
    ren.SetColor(1, 1, 1);
    kit::Centred(ink, fitted, x + width / 2, y + (height - ink.LineHeight()) / 2);
}

/**
 * @brief The side's banner colours for dark slate panels; black and navy
 * sides (the Lich King's) are lifted to readable tones.
 */
const Font& SideBannerFont(const Font& big_font, const w2::Side& side) {
    if (side.Colour() == 0) return big_font.Colours(15, side.Edge() == 0 ? 9 : side.Edge());
    if (side.Colour() == 6) return big_font.Colours(5, 0);
    return big_font.Colours(side.Colour(), side.Edge());
}

void OpenReport(w2::UiContext& ctx);
void OpenAction(w2::UiContext& ctx);

/**
 * @brief The modern Diplomatic Report: the classic one plus alliances, and
 * a click on a side opens the Parley.
 */
class ReportDialog final : public kit::Modal {
  public:
    explicit ReportDialog(w2::UiContext& ctx)
            : kit::Modal(ctx), view_(kit::View(ctx, kReportDialog)) {
        SetDialogView(&view_);
        view_.state[kDone] = w2::uidata::kNormal;
        view_.state[kAction] = w2::uidata::kNormal;
        w2::Game& game = ctx.ActiveGame();
        titles_ = dip::Ratings(game);
        for (w2::Side* side : game.Sides()) {
            if (side->IsAlive()) order_.push_back(side);
        }
        w2::LuaSort(order_, [](w2::Side* lhs, w2::Side* rhs) {
            if (lhs->DiploScore() != rhs->DiploScore()) {
                return lhs->DiploScore() < rhs->DiploScore();
            }
            return lhs->Index() < rhs->Index();
        });
    }

    /** @brief Draws the grid of stances (allies marked), the ranking and a hint. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0x6b, 0), 320, 64);
        kit::SetPal(ctx, 1);
        ren.Rectangle(gfx::kFill, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) ren.Rectangle(gfx::kFill, 120 + 32 * i, 110, 1, 261);
        ren.Rectangle(gfx::kFill, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) ren.Rectangle(gfx::kFill, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(game, i)) {
                SmallShield(ctx, i, 128 + 32 * i, 117);
                SmallShield(ctx, i, 96, 146 + 29 * i);
            }
        }
        const Font& tiny_font = kit::GetFont(ctx, 0);
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col;
                int y = 139 + 29 * row;
                if (col == row) {
                    Diplom(ctx, 384, 29, 32, 29, x, y);
                } else if (Playing(game, col) && Playing(game, row)) {
                    int state = dip::State(game, col, row);
                    if (state == dip::kWar) {
                        Diplom(ctx, 384, 0, 32, 29, x, y);
                    } else if (state == dip::kPeace) {
                        Diplom(ctx, 320, 0, 32, 29, x, y);
                        if (dip::IsAllied(game, col, row)) {
                            kit::SetPal(ctx, 10);
                            kit::Outline(ctx, x + 1, y + 1, 30, 27);
                            ren.SetColor(1, 1, 1);
                            tiny_font.Colours(7, 0).Draw("ALLY", x + 3, y + 2);
                        }
                    }
                }
            }
        }
        // Recessed ratings panel on the right
        kit::SetPal(ctx, 4);
        ren.Rectangle(gfx::kFill, 392, 110, 160, 262);
        kit::Bevel(ctx, 392, 110, 160, 262, 0, 1);
        const Font& font = kit::GetFont(ctx, 2);
        ren.SetColor(1, 1, 1);
        kit::Centred(font.Colours(7, 0), kit::Text(ctx, 0x6d, 0), 472, 116);
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kFill, 394, 138, 156, 1);
        for (size_t i = 0; i < order_.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(ctx, order_[i]->Index(), 400, y);
            ren.SetColor(1, 1, 1);
            std::string title = FitText(font, titles_[order_[i]->Index()], 120, false);
            font.Draw(title, 424, y);
        }
        // Centred between ACTION (96..160) and DONE (480..544)
        ren.SetColor(1, 1, 1);
        kit::Centred(tiny_font.Colours(7, 0), "Click any shield to Parley  -  F10: AI Brain", 320,
                     384);
        kit::DrawControls(ctx, view_);
    }

    /**
     * @brief A click on another side's shield or ranking row opens the
     * Parley with it; Done closes, Action goes on to Diplomatic Action.
     */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        for (int i = 0; i < 8; i++) {
            if (i == viewer || !Playing(game, i)) continue;
            kit::Rect top_shield{120 + 32 * i, 110, 32, 28};
            kit::Rect left_shield{88, 139 + 29 * i, 32, 29};
            if (top_shield.Contains(x, y) || left_shield.Contains(x, y)) {
                PopSelf();
                OpenParley(ctx, i, false);
                return;
            }
        }
        for (size_t i = 0; i < order_.size(); i++) {
            int side_index = order_[i]->Index();
            if (side_index == viewer) continue;
            kit::Rect rate_row{394, 142 + 29 * static_cast<int>(i), 156, 26};
            if (rate_row.Contains(x, y)) {
                PopSelf();
                OpenParley(ctx, side_index, false);
                return;
            }
        }
        const auto* control = kit::ControlAt(view_, x, y);
        if (!control) return;
        if (control->id == kDone) {
            PopSelf();
        } else if (control->id == kAction) {
            PopSelf();
            OpenAction(ctx);
        }
    }

    /** @brief F10 opens the AI Brain Inspector; Return and Escape close. */
    void Keypressed(const std::string& key) override {
        if (key == "f10") {
            OpenBrainInspector(Ctx());
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View view_;  ///< Dialog 21 and its controls' states.
    TitleMap titles_;    ///< Each side's diplomatic title, by side.
    SideList order_;     ///< The living sides, by rating.
};

/** @brief Opens the modern Diplomatic Report. */
void OpenReport(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<ReportDialog>(ctx));
}

/**
 * @brief The modern Diplomatic Action: the classic one plus alliances,
 * truces and gold offers; a click on a side's shield opens the Parley.
 */
class ActionDialog final : public kit::Modal {
  public:
    explicit ActionDialog(w2::UiContext& ctx)
            : kit::Modal(ctx), view_(kit::View(ctx, kActionDialog)) {
        SetDialogView(&view_);
        int viewer = ActiveViewerIndex(ctx, ctx.ActiveGame());
        for (const auto& control : view_.dialog.controls) {
            view_.state[control.id] = w2::uidata::kNormal;
        }
        for (int i = 0; i < 8; i++) {
            if (i != viewer) others_.push_back(i);
        }
    }

    /** @brief Draws the player's side and a column per other side. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        const w2::Side& my_side = game.GetMap().Sides()[viewer];
        kit::Popup(ctx, kPopupRect);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), kit::Text(ctx, 0x6c, 0), 320, 64);
        kit::Shield(ctx, viewer, 96, 111);
        const Font& font = kit::GetFont(ctx, 2);
        const Font& tiny_font = kit::GetFont(ctx, 0);
        ren.SetColor(1, 1, 1);
        SideBannerFont(font, my_side).Draw(FitText(font, my_side.Name(), 122, false), 144, 122);
        for (int i = 0; i < 7; i++) font.Draw(kit::Text(ctx, 0x6e, i), 104, kActionRowYs[i]);
        for (size_t col = 0; col < others_.size(); col++) {
            int other = others_[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::Shield(ctx, other, x, 111);
            if (!Playing(game, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(ctx, x, y);
            } else {
                int state = dip::State(game, other, viewer);
                Diplom(ctx, 80 + 120 * state, 45, 40, 40, x, 151);
                if (dip::IsAllied(game, other, viewer)) {
                    kit::SetPal(ctx, 10);
                    kit::Outline(ctx, x + 1, 152, 38, 38);
                    ren.SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw("ALLY", x + 6, 153);
                }
                int truce_turns = dip::TruceTurns(game, viewer, other);
                if (truce_turns > 0) {
                    ren.SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw(absl::StrFormat("T:%d", truce_turns), x + 8, 177);
                }

                int theirs = dip::Proposal(game, other, viewer);
                bool their_ally = dip::ProposalAllied(game, other, viewer);
                if (theirs == state && (!their_ally || dip::IsAllied(game, other, viewer))) {
                    Blank(ctx, x, 191);
                } else {
                    Diplom(ctx, 80 + 120 * theirs, 45, 40, 40, x, 191);
                    if (their_ally) {
                        ren.SetColor(1, 1, 1);
                        tiny_font.Colours(7, 0).Draw("ALLY", x + 6, 193);
                    }
                }

                int mine = dip::Proposal(game, viewer, other);
                bool my_ally = dip::ProposalAllied(game, viewer, other);
                for (int stance = 0; stance <= 2; stance++) {
                    Diplom(ctx, 120 * stance + (mine == stance ? 40 : 0), 45, 40, 40, x,
                           245 + 40 * stance);
                }
                if (my_ally && mine == dip::kPeace) {
                    kit::SetPal(ctx, 10);
                    kit::Outline(ctx, x + 1, 246, 38, 38);
                    ren.SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw("ALLY", x + 6, 247);
                }
                int gold_offer = dip::GoldOffer(game, viewer, other);
                if (gold_offer > 0) {
                    ren.SetColor(1, 1, 1);
                    tiny_font.Colours(7, 0).Draw(absl::StrFormat("+%dg", gold_offer), x + 3, 271);
                }
            }
        }
        // Centred between BACK (96..160) and OK (480..544)
        ren.SetColor(1, 1, 1);
        kit::Centred(tiny_font.Colours(7, 0), "Click a top shield to Parley  -  F10: AI Brain", 320,
                     384);
        kit::ControlIdSet offer_controls;
        for (int stance = 0; stance <= 2; stance++) {
            for (int i = 0; i <= 6; i++) offer_controls.insert(kOffer[stance] + i);
        }
        kit::DrawControls(ctx, view_, offer_controls);
    }

    /**
     * @brief A click on a side's shield opens the Parley; OK closes, Back
     * returns to the report. An offer proposes that stance; offering peace
     * where peace is already proposed toggles proposing an alliance.
     */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        for (size_t col = 0; col < others_.size(); col++) {
            int other = others_[col];
            if (!Playing(game, other)) continue;
            kit::Rect shield_rect{272 + 40 * static_cast<int>(col), 108, 40, 43};
            if (shield_rect.Contains(x, y)) {
                PopSelf();
                OpenParley(ctx, other, true);
                return;
            }
        }
        const auto* control = kit::ControlAt(view_, x, y);
        if (!control) return;
        if (control->id == kOk) {
            PopSelf();
            return;
        }
        if (control->id == kBack) {
            PopSelf();
            OpenReport(ctx);
            return;
        }
        for (int stance = 0; stance <= 2; stance++) {
            int column = control->id - kOffer[stance];
            if (column >= 0 && column < 7 && column < static_cast<int>(others_.size())) {
                int other = others_[column];
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
            OpenBrainInspector(Ctx());
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") PopSelf();
    }

  private:
    screen::View view_;     ///< Dialog 22 and its controls' states.
    SideIndexList others_;  ///< The other seven sides, one column each.
};

/** @brief Opens the modern Diplomatic Action, a column for each side but the viewer. */
void OpenAction(w2::UiContext& ctx) {
    kit::Push(ctx, std::make_shared<ActionDialog>(ctx));
}

// ------------------------------------------------------------------ Parley & Attitude Ledger
/**
 * @brief The Parley with one rival: its ruler and stance, the ledger of what
 * shapes its attitude, the treaty and tribute controls, and a forecast of
 * what it would take to win it over.
 */
class ParleyModal final : public kit::Modal {
  public:
    ParleyModal(w2::UiContext& ctx, int other_side, bool return_to_action)
            : kit::Modal(ctx), other_(other_side), return_to_action_(return_to_action) {}

    /**
     * @brief Draws the Parley; a rival that is no longer playing gives way to
     * the first one that is.
     */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        if (!Playing(game, other_) || other_ == viewer) {
            SideIndexList fallback_rivals = RivalList(game, viewer);
            if (!fallback_rivals.empty()) other_ = fallback_rivals[0];
        }
        const w2::Side& other_side = game.GetMap().Sides()[other_];
        const w2::Side& my_side = game.GetMap().Sides()[viewer];
        const auto& profile = w2::ai::personality::ForSide(other_side);
        auto attitude = dip::ComputeAttitude(game, other_, viewer);

        kit::Popup(ctx, kBox);
        const Font& big_font = kit::GetFont(ctx, 2);
        const Font& small_font = kit::GetFont(ctx, 0);

        // Top Title & Clickable Rival Switcher Shields
        ren.SetColor(1, 1, 1);
        big_font.Colours(7, 0).Draw("DIPLOMATIC PARLEY", 74, 50);

        SideIndexList rivals = RivalList(game, viewer);
        int shields_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            int rival = rivals[idx];
            int shield_x = shields_x + static_cast<int>(idx) * 24;
            if (rival == other_) {
                kit::SetPal(ctx, 7);
                ren.Rectangle(gfx::kFill, shield_x - 2, 49, 20, 20);
                kit::SetPal(ctx, 0);
                kit::Outline(ctx, shield_x - 3, 48, 22, 22);
            }
            SmallShield(ctx, rival, shield_x, 51);
        }

        // Ruler Bio & Stance Banner (72, 73, 496 x 64)
        DrawSunkenPanel(ctx, 72, 73, 496, 64);
        kit::Shield(ctx, other_, 80, 85);

        std::string archetype_tag = "[" + CleanText(profile.archetype) + "]";
        int archetype_width = small_font.Width(archetype_tag);
        int max_title_width = 432 - archetype_width - 10;
        std::string side_label = CleanText(other_side.Name(), false);
        int side_width = big_font.Width(side_label);
        ren.SetColor(1, 1, 1);
        SideBannerFont(big_font, other_side).Draw(side_label, 128, 77);
        if (side_width + 16 < max_title_width) {
            std::string ruler_part =
                    FitText(big_font, " - " + profile.name, max_title_width - side_width, false);
            big_font.Draw(ruler_part, 128 + side_width, 77);
        }
        kit::Right(small_font.Colours(7, 0), archetype_tag, 560, 79);

        ren.SetColor(1, 1, 1);
        small_font.Colours(5, 0).Draw(FitText(small_font, "\"" + profile.motto + "\"", 432), 128,
                                      97);

        int state = dip::State(game, viewer, other_);
        bool allied = dip::IsAllied(game, viewer, other_);
        std::string state_name =
                allied ? "ALLIED"
                       : (state == dip::kPeace
                                  ? "PEACE"
                                  : (state == dip::kIntermediate ? "UNEASY" : "AT WAR"));
        int state_colour =
                (allied || state == dip::kPeace) ? 10 : (state == dip::kIntermediate ? 7 : 8);
        int truce_turns = dip::TruceTurns(game, viewer, other_);
        int attitude_colour =
                attitude.total_score >= 15 ? 10 : (attitude.total_score <= -15 ? 8 : 7);

        int text_x = 128;
        text_x += small_font.Colours(state_colour, 0).Draw("[" + state_name + "]", text_x, 115);
        std::string mid_meta = absl::StrFormat("  Truce:%d  Trust:%+d  Attitude:", truce_turns,
                                               attitude.trust_score);
        text_x += small_font.Colours(1, 0).Draw(mid_meta, text_x, 115);
        std::string attitude_tail =
                FitText(small_font,
                        absl::StrFormat(" %+d (%s)", attitude.total_score, attitude.stance_summary),
                        std::max(20, 560 - text_x));
        small_font.Colours(attitude_colour, 0).Draw(attitude_tail, text_x, 115);

        // Left Panel: Attitude Ledger (72, 143, 300 x 167)
        int net_colour = attitude.total_score > 0 ? 10 : (attitude.total_score < 0 ? 8 : 7);
        DrawSunkenPanel(ctx, 72, 143, 300, 167, "ATTITUDE LEDGER",
                        absl::StrFormat("Total: %+d", attitude.total_score), net_colour);

        ren.SetScissor(74, 163, 296, 145);
        if (attitude.factors.empty()) {
            small_font.Colours(1, 0).Draw("No active grievances or modifiers.", 82, 172);
        } else {
            int factor_y = 166;
            for (const auto& factor : attitude.factors) {
                if (factor_y > 294) break;
                std::string delta_text = absl::StrFormat("%+d", factor.delta);
                int delta_colour = factor.delta > 0 ? 10 : (factor.delta < 0 ? 8 : 1);
                kit::Right(small_font.Colours(delta_colour, 0), delta_text, 110, factor_y);

                LineList factor_lines = WrapText(small_font, factor.label, 246);
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
        ren.SetScissor();

        // Right Panel: Treaty & Tribute Controls (380, 143, 188 x 167)
        DrawSunkenPanel(ctx, 380, 143, 188, 167, "PROPOSE TREATY");

        int my_proposal = dip::Proposal(game, viewer, other_);
        bool my_alliance_proposal = dip::ProposalAllied(game, viewer, other_);
        int gold_offer = dip::GoldOffer(game, viewer, other_);
        int war_target = dip::WarTarget(game, viewer, other_);
        std::string war_target_name =
                (war_target >= 0 && war_target < 8 && Playing(game, war_target))
                        ? game.GetMap().Sides()[war_target].Name()
                        : "None";

        DrawButton(ctx, kBtnAlly.x, kBtnAlly.y, kBtnAlly.w, kBtnAlly.h,
                   my_alliance_proposal ? "* Propose Alliance *" : "Propose Alliance",
                   my_alliance_proposal, false, 11);
        DrawButton(ctx, kBtnPeace.x, kBtnPeace.y, kBtnPeace.w, kBtnPeace.h,
                   (!my_alliance_proposal && my_proposal == dip::kPeace) ? "* Propose Peace *"
                                                                         : "Propose Peace",
                   !my_alliance_proposal && my_proposal == dip::kPeace, false, 11);
        DrawButton(ctx, kBtnUneasy.x, kBtnUneasy.y, kBtnUneasy.w, kBtnUneasy.h,
                   my_proposal == dip::kIntermediate ? "* Propose Uneasy *" : "Propose Uneasy",
                   my_proposal == dip::kIntermediate, false, 13);
        DrawButton(ctx, kBtnWar.x, kBtnWar.y, kBtnWar.w, kBtnWar.h,
                   my_proposal == dip::kWar ? "* Propose War *" : "Declare / Propose War",
                   my_proposal == dip::kWar, false, 9);

        DrawButton(ctx, kBtnGoldMinus.x, kBtnGoldMinus.y, kBtnGoldMinus.w, kBtnGoldMinus.h,
                   "Gift -50g", false, gold_offer <= 0, 13);
        DrawButton(ctx, kBtnGoldPlus.x, kBtnGoldPlus.y, kBtnGoldPlus.w, kBtnGoldPlus.h,
                   absl::StrFormat("Gift +%dg", gold_offer + 50), gold_offer > 0,
                   gold_offer + 50 > my_side.Gold(), 13);

        DrawButton(ctx, kBtnJointWar.x, kBtnJointWar.y, kBtnJointWar.w, kBtnJointWar.h,
                   "War Target: " + war_target_name, war_target != w2::kNone, false, 9);

        // Bottom Panel: Diplomatic Forecast (72, 316, 496 x 72)
        DrawSunkenPanel(
                ctx, 72, 316, 496, 72, "DIPLOMATIC FORECAST",
                absl::StrFormat("Treasury: %d gp   Gift Offer: %d gp", my_side.Gold(), gold_offer),
                5);

        ren.SetScissor(74, 336, 492, 50);
        LineList wrapped = WrapText(small_font, attitude.what_it_takes, 478);
        int forecast_y = 339;
        for (size_t i = 0; i < wrapped.size() && i < 2; i++) {
            small_font.Draw(wrapped[i], 80, forecast_y);
            forecast_y += 14;
        }
        if (allied && dip::AlliedTargetCity(game, other_) != w2::kNone) {
            if (const w2::City* target_city =
                        game.GetMap().CityAt(dip::AlliedTargetCity(game, other_))) {
                std::string target_text = FitText(
                        small_font,
                        absl::StrFormat("Allied War Target: %s (%d, %d)", target_city->Name(),
                                        target_city->X(), target_city->Y()),
                        478);
                small_font.Colours(10, 0).Draw(target_text, 80, 369);
            }
        } else if (forecast_y <= 368) {
            int their_proposal = dip::Proposal(game, other_, viewer);
            bool their_ally = dip::ProposalAllied(game, other_, viewer);
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
        ren.SetScissor();

        // Footer Hints & Action Buttons
        small_font.Colours(1, 0).Draw("Left/Right or click top shields to switch", 74, 401);
        DrawButton(ctx, kBtnBrain.x, kBtnBrain.y, kBtnBrain.w, kBtnBrain.h, "AI Brain (F10)");
        DrawButton(ctx, kBtnBack.x, kBtnBack.y, kBtnBack.w, kBtnBack.h, "Back");
    }

    /**
     * @brief Handles a click: a rival's shield switches to it; the treaty,
     * gold and war target buttons change what the viewer proposes.
     */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        SideIndexList rivals = RivalList(game, viewer);
        int shields_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            kit::Rect shield{shields_x + static_cast<int>(idx) * 24 - 2, 49, 20, 20};
            if (shield.Contains(x, y)) {
                other_ = rivals[idx];
                return;
            }
        }
        if (kBtnAlly.Contains(x, y)) {
            dip::Propose(game, viewer, other_, dip::kPeace);
            dip::ProposeAlliance(game, viewer, other_, true);
        } else if (kBtnPeace.Contains(x, y)) {
            dip::Propose(game, viewer, other_, dip::kPeace);
            dip::ProposeAlliance(game, viewer, other_, false);
        } else if (kBtnUneasy.Contains(x, y)) {
            dip::Propose(game, viewer, other_, dip::kIntermediate);
        } else if (kBtnWar.Contains(x, y)) {
            dip::Propose(game, viewer, other_, dip::kWar);
            dip::SetGoldOffer(game, viewer, other_, 0);
        } else if (kBtnGoldMinus.Contains(x, y)) {
            int offer = dip::GoldOffer(game, viewer, other_);
            dip::SetGoldOffer(game, viewer, other_, std::max(0, offer - 50));
        } else if (kBtnGoldPlus.Contains(x, y)) {
            int offer = dip::GoldOffer(game, viewer, other_);
            if (offer + 50 <= game.GetMap().Sides()[viewer].Gold()) {
                dip::SetGoldOffer(game, viewer, other_, offer + 50);
            }
        } else if (kBtnJointWar.Contains(x, y)) {
            int target = dip::WarTarget(game, viewer, other_);
            int next = w2::kNone;
            int start = (target == w2::kNone) ? 0 : target + 1;
            for (int side = start; side < 8; side++) {
                if (side != viewer && side != other_ && Playing(game, side)) {
                    next = side;
                    break;
                }
            }
            dip::SetWarTarget(game, viewer, other_, next);
        } else if (kBtnBrain.Contains(x, y)) {
            OpenBrainInspector(ctx, other_);
        } else if (kBtnBack.Contains(x, y)) {
            bool back_to_action = return_to_action_;
            PopSelf();
            if (back_to_action) {
                OpenAction(ctx);
            } else {
                OpenReport(ctx);
            }
        }
    }

    /**
     * @brief Left and Right switch rivals, F10 opens the AI Brain Inspector,
     * Return and Escape go back.
     */
    void Keypressed(const std::string& key) override {
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        if (key == "left" || key == "right") {
            SideIndexList rivals = RivalList(game, viewer);
            if (!rivals.empty()) {
                auto found = std::find(rivals.begin(), rivals.end(), other_);
                int position =
                        (found == rivals.end()) ? 0 : static_cast<int>(found - rivals.begin());
                int count = static_cast<int>(rivals.size());
                other_ = rivals[(position + (key == "right" ? 1 : count - 1)) % count];
            }
            return;
        }
        if (key == "f10") {
            OpenBrainInspector(ctx, other_);
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") {
            bool back_to_action = return_to_action_;
            PopSelf();
            if (back_to_action) {
                OpenAction(ctx);
            } else {
                OpenReport(ctx);
            }
        }
    }

  private:
    static constexpr kit::Rect kBox{60, 44, 520, 384};
    static constexpr kit::Rect kBtnAlly{387, 166, 174, 21};
    static constexpr kit::Rect kBtnPeace{387, 189, 174, 21};
    static constexpr kit::Rect kBtnUneasy{387, 212, 174, 21};
    static constexpr kit::Rect kBtnWar{387, 235, 174, 21};
    static constexpr kit::Rect kBtnGoldMinus{387, 260, 84, 21};
    static constexpr kit::Rect kBtnGoldPlus{477, 260, 84, 21};
    static constexpr kit::Rect kBtnJointWar{387, 283, 174, 21};
    static constexpr kit::Rect kBtnBrain{352, 395, 116, 25};
    static constexpr kit::Rect kBtnBack{476, 395, 92, 25};

    /** @brief The sides still playing other than the viewer, in index order. */
    SideIndexList RivalList(const w2::Game& game, int viewer) const {
        SideIndexList out;
        for (int i = 0; i < 8; i++) {
            if (i != viewer && Playing(game, i)) out.push_back(i);
        }
        return out;
    }

    int other_ = 0;                  ///< The rival parleyed with.
    bool return_to_action_ = false;  ///< Back returns to Diplomatic Action, else the report.
};

// ------------------------------------------------------------------ Turn-Start Envoy Missive Modal
/**
 * @brief A missive from another side's envoy at the start of the player's
 * turn: what it proposes or demands, and why. Accept, open the Parley, or
 * decline; then the next missive shows.
 */
class MissiveModal final : public kit::Modal {
  public:
    MissiveModal(w2::UiContext& ctx, w2::DiplomaticMissive missive, kit::Done after)
            : kit::Modal(ctx), missive_(std::move(missive)), after_(std::move(after)) {}

    /** @brief Draws the envoy's banner, the message and its context. */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        const w2::Side& sender = game.GetMap().Sides()[std::clamp(missive_.from_side, 0, 7)];
        kit::Popup(ctx, kBox);

        const Font& big_font = kit::GetFont(ctx, 2);
        const Font& small_font = kit::GetFont(ctx, 0);

        // Top Envoy Banner
        DrawSunkenPanel(ctx, 100, 84, 440, 56);
        kit::Shield(ctx, missive_.from_side, 108, 92);

        ren.SetColor(1, 1, 1);
        SideBannerFont(big_font, sender)
                .Draw(FitText(big_font, missive_.headline, 372, false), 156, 90);

        std::string deal_tag = missive_.propose_alliance
                                       ? "Proposal: Military Alliance (Shared Vision)"
                                       : (missive_.proposed_state == dip::kPeace
                                                  ? "Proposal: Peace Treaty"
                                                  : (missive_.proposed_state == dip::kIntermediate
                                                             ? "Notice: Border Warning / Uneasy"
                                                             : "Declaration"));
        if (missive_.gold_demand > 0) {
            deal_tag += absl::StrFormat("  -  Tribute Demand: %d gp", missive_.gold_demand);
        } else if (missive_.gold_demand < 0) {
            deal_tag += absl::StrFormat("  -  Tribute Gift: +%d gp", -missive_.gold_demand);
        }
        int deal_colour = (missive_.propose_alliance || missive_.proposed_state == dip::kPeace)
                                  ? 10
                                  : (missive_.proposed_state == dip::kIntermediate ? 7 : 8);
        small_font.Colours(deal_colour, 0).Draw(FitText(small_font, deal_tag, 372), 156, 114);

        // Envoy Message Box
        DrawSunkenPanel(ctx, 100, 146, 440, 108, "ROYAL ENVOY DISPATCH");
        ren.SetScissor(102, 166, 436, 86);
        LineList body_lines = WrapText(big_font, missive_.body, 420, false);
        int body_y = 170;
        for (const auto& line : body_lines) {
            if (body_y > 236) break;
            big_font.Draw(line, 108, body_y);
            body_y += 19;
        }
        ren.SetScissor();

        // Diplomatic Context Box
        int my_gold = ctx.Player() ? ctx.Player()->Gold() : 0;
        DrawSunkenPanel(ctx, 100, 260, 440, 78, "DIPLOMATIC CONTEXT",
                        absl::StrFormat("Your Treasury: %d gp", my_gold), 5);
        ren.SetScissor(102, 280, 436, 56);
        LineList context_lines = WrapText(small_font, missive_.reason_summary, 420);
        int context_y = 283;
        for (const auto& line : context_lines) {
            if (context_y > 322) break;
            small_font.Draw(line, 108, context_y);
            context_y += 14;
        }
        ren.SetScissor();

        bool can_afford = (missive_.gold_demand <= 0 || my_gold >= missive_.gold_demand);
        DrawButton(ctx, kBtnAccept.x, kBtnAccept.y, kBtnAccept.w, kBtnAccept.h,
                   missive_.gold_demand > 0
                           ? absl::StrFormat("Pay %dg + Accept", missive_.gold_demand)
                           : "Accept Proposal",
                   can_afford, !can_afford, 11);
        DrawButton(ctx, kBtnParley.x, kBtnParley.y, kBtnParley.w, kBtnParley.h, "Open Parley");
        DrawButton(ctx, kBtnDecline.x, kBtnDecline.y, kBtnDecline.w, kBtnDecline.h, "Decline",
                   false, false, 9);
    }

    /**
     * @brief Accept pays any tribute and makes the proposed stance mutual
     * (with a truce and trust for peace or an alliance, and war on any joint
     * target); Open Parley goes to the sender; Decline moves on.
     */
    void Mousepressed(int x, int y, int) override {
        w2::UiContext& ctx = Ctx();
        w2::Game& game = ctx.ActiveGame();
        int viewer = ActiveViewerIndex(ctx, game);
        if (kBtnAccept.Contains(x, y)) {
            if (missive_.gold_demand > 0 &&
                game.GetMap().Sides()[viewer].Gold() < missive_.gold_demand) {
                return;
            }
            if (missive_.gold_demand > 0) {
                game.GetMap().Sides()[viewer].SpendGold(missive_.gold_demand);
                game.GetMap().Sides()[missive_.from_side].AddGold(missive_.gold_demand);
            }
            dip::Propose(game, viewer, missive_.from_side, missive_.proposed_state);
            dip::Propose(game, missive_.from_side, viewer, missive_.proposed_state);
            if (missive_.propose_alliance) {
                dip::SetAllied(game, viewer, missive_.from_side, true);
                dip::SetTruce(game, viewer, missive_.from_side, 15);
                dip::AddTrust(game, missive_.from_side, viewer, 18);
            } else if (missive_.proposed_state == dip::kPeace) {
                game.Diplomacy().SetSymmetricState(w2::SideId(viewer),
                                                   w2::SideId(missive_.from_side), dip::kPeace);
                dip::SetTruce(game, viewer, missive_.from_side, 10);
                dip::AddTrust(game, missive_.from_side, viewer, 10);
            }
            if (missive_.joint_war_target != w2::kNone &&
                Playing(game, missive_.joint_war_target)) {
                game.Diplomacy().SetSymmetricState(
                        w2::SideId(viewer), w2::SideId(missive_.joint_war_target), dip::kWar);
                dip::Propose(game, viewer, missive_.joint_war_target, dip::kWar);
            }
            ctx.GetViewport().StratDirty();
            Finish();
        } else if (kBtnParley.Contains(x, y)) {
            int sender = missive_.from_side;
            PopSelf();
            OpenParley(ctx, sender, false);
        } else if (kBtnDecline.Contains(x, y)) {
            Finish();
        }
    }

    /** @brief Escape declines; Return accepts, as a click on Accept. */
    void Keypressed(const std::string& key) override {
        if (key == "escape") {
            Finish();
        } else if (key == "return" || key == "kpenter") {
            Mousepressed(kBtnAccept.x + 5, kBtnAccept.y + 5, 1);
        }
    }

  private:
    static constexpr kit::Rect kBox{88, 72, 464, 316};
    static constexpr kit::Rect kBtnAccept{100, 348, 148, 28};
    static constexpr kit::Rect kBtnParley{258, 348, 136, 28};
    static constexpr kit::Rect kBtnDecline{404, 348, 136, 28};

    /** @brief Closes this missive and shows the next one, if any. */
    void Finish() {
        w2::UiContext& ctx = Ctx();
        kit::Done done = std::move(after_);
        PopSelf();
        PollMissives(ctx, std::move(done));
    }

    w2::DiplomaticMissive missive_;  ///< The missive shown.
    kit::Done after_;                ///< Called once no missive is left.
};

// ------------------------------------------------------------------ F10 AI Brain & Policy
// Inspector
/**
 * @brief The AI Brain Inspector: for one side its personality traits, what
 * it believes about its rivals, and its recent policy decisions; it can also
 * switch the side between the classic AI and the learnable one.
 */
class BrainInspectorModal final : public kit::Modal {
  public:
    BrainInspectorModal(w2::UiContext& ctx, int side_idx) : kit::Modal(ctx), side_idx_(side_idx) {}

    /**
     * @brief Draws the inspector; a side that is no longer playing gives way
     * to the first computer still playing, else the viewer.
     */
    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        if (!Playing(game, side_idx_)) {
            for (int candidate = 0; candidate < 8; candidate++) {
                if (Playing(game, candidate) &&
                    game.GetMap().Sides()[candidate].IsComputerControlled()) {
                    side_idx_ = candidate;
                    break;
                }
            }
            if (!Playing(game, side_idx_)) side_idx_ = ActiveViewerIndex(ctx, game);
        }
        w2::Side& side = game.GetMap().Sides()[side_idx_];
        const auto& profile = w2::ai::personality::ForSide(side);
        const auto& belief = w2::ai::belief::Ensure(side);

        kit::Popup(ctx, kBox);
        const Font& big_font = kit::GetFont(ctx, 2);
        const Font& small_font = kit::GetFont(ctx, 0);

        ren.SetColor(1, 1, 1);
        big_font.Colours(7, 0).Draw("AI BRAIN INSPECTOR (F10)", 56, 42);

        // Top row of clickable faction shields
        for (int i = 0; i < 8; i++) {
            if (!Playing(game, i)) continue;
            int shield_x = 376 + i * 26;
            if (i == side_idx_) {
                kit::SetPal(ctx, 7);
                ren.Rectangle(gfx::kFill, shield_x - 2, 41, 20, 20);
                kit::SetPal(ctx, 0);
                kit::Outline(ctx, shield_x - 3, 40, 22, 22);
            }
            SmallShield(ctx, i, shield_x, 43);
        }

        // Faction Bio & Value Head Summary Banner (56, 67, 528 x 58)
        DrawSunkenPanel(ctx, 56, 67, 528, 58);
        kit::Shield(ctx, side_idx_, 64, 76);

        std::string side_title = CleanText(side.Name(), false);
        int title_width = big_font.Width(side_title);
        ren.SetColor(1, 1, 1);
        SideBannerFont(big_font, side).Draw(FitText(big_font, side_title, 260, false), 112, 71);
        if (title_width + 16 < 262) {
            big_font.Draw(FitText(big_font, " - " + profile.name, 262 - title_width, false),
                          112 + title_width, 71);
        }
        small_font.Colours(5, 0).Draw(FitText(small_font, "\"" + profile.motto + "\"", 464), 112,
                                      91);

        int explored_percent =
                static_cast<int>(std::round((1.0F - belief.frontier_ratio) * 100.0F));
        float board_val = side.ComputerPlayer() ? side.ComputerPlayer()->LastBoardValue() : 0.0F;
        std::string evaluation = absl::StrFormat(
                "[%s]  V(s):%+.2f  Expl:%d%%  Cit:%d  Gold:%d gp", profile.archetype, board_val,
                explored_percent, static_cast<int>(belief.visible_cities.size()), side.Gold());
        int value_colour = board_val >= 0.05F ? 10 : (board_val <= -0.05F ? 8 : 7);
        small_font.Colours(value_colour, 0).Draw(FitText(small_font, evaluation, 464), 112, 106);

        DrawButton(ctx, kBtnToggleEngine.x, kBtnToggleEngine.y, kBtnToggleEngine.w,
                   kBtnToggleEngine.h, side.IsClassicAi() ? "AI: 1993 Classic" : "AI: Learnable RL",
                   !side.IsClassicAi(), false, 11);
        DrawButton(ctx, kBtnClose.x, kBtnClose.y, kBtnClose.w, kBtnClose.h, "Close");

        // Left Panel: 10D Personality Vector Trait Bars (56, 130, 212 x 165)
        DrawSunkenPanel(ctx, 56, 130, 212, 165, "10D PERSONALITY TRAITS");
        for (int trait = 0; trait < 10; trait++) {
            int trait_y = 150 + trait * 14;
            small_font.Draw(kTraitLabels[trait], 64, trait_y);
            int bar_x = 160;
            int bar_w = 98;
            int mid_x = bar_x + bar_w / 2;
            kit::SetPal(ctx, 3);
            ren.Rectangle(gfx::kFill, bar_x, trait_y + 3, bar_w, 8);
            kit::Bevel(ctx, bar_x, trait_y + 3, bar_w, 8, 0, 2);
            float value = std::clamp(profile.traits[trait], -1.0F, 1.0F);
            int fill_w = static_cast<int>(std::round(std::abs(value) * (bar_w / 2 - 2)));
            kit::SetPal(ctx, value >= 0 ? 10 : 8);
            if (fill_w > 0) {
                if (value >= 0) {
                    ren.Rectangle(gfx::kFill, mid_x + 1, trait_y + 4, fill_w, 6);
                } else {
                    ren.Rectangle(gfx::kFill, mid_x - fill_w, trait_y + 4, fill_w, 6);
                }
            }
            kit::SetPal(ctx, 15);
            ren.Rectangle(gfx::kFill, mid_x, trait_y + 2, 1, 10);
        }

        // Right Panel: Fair-Information Belief Summary (276, 130, 308 x 165)
        DrawSunkenPanel(
                ctx, 276, 130, 308, 165, "BELIEF MEMORY",
                absl::StrFormat("Fog:%d Stacks:%d", static_cast<int>(belief.fog_frontiers.size()),
                                static_cast<int>(belief.stacks_by_tile.size())),
                5);
        ren.SetScissor(278, 150, 304, 143);
        small_font.Colours(1, 0).Draw("Rival", 284, 151);
        kit::Right(small_font.Colours(1, 0), "Cit", 434, 151);
        kit::Right(small_font.Colours(1, 0), "Str", 478, 151);
        kit::Right(small_font.Colours(1, 0), "Us / Them", 574, 151);
        kit::SetPal(ctx, 0);
        ren.Rectangle(gfx::kFill, 282, 165, 296, 1);

        int rival_y = 169;
        for (int rival = 0; rival < 8; rival++) {
            if (rival == side_idx_ || !Playing(game, rival)) continue;
            if (rival_y > 279) break;
            SmallShield(ctx, rival, 284, rival_y - 1);
            std::string rival_name = FitText(small_font, game.GetMap().Sides()[rival].Name(), 106);
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
        ren.SetScissor();

        // Bottom Panel: Top Scored Neural Policy Decisions (56, 301, 528 x 135)
        DrawSunkenPanel(ctx, 56, 301, 528, 135, "RECENT NEURAL POLICY DECISIONS",
                        "Left/Right: Switch Side", 1);
        ren.SetScissor(58, 321, 524, 113);
        auto thoughts = side.ComputerPlayer() ? side.ComputerPlayer()->Thoughts()
                                              : w2::IComputerPlayer::ThoughtSpan{};
        if (thoughts.empty()) {
            small_font.Colours(1, 0).Draw(
                    "No neural thoughts recorded yet (advance a turn to watch this AI act).", 66,
                    330);
        } else {
            int thought_y = 323;
            int shown = 0;
            // the other thoughts first, then the diplomatic ones
            for (int pass = 0; pass < 2; pass++) {
                for (const auto& thought : thoughts) {
                    bool is_diplo = (thought.category == "Diplomacy");
                    if ((pass == 0 && is_diplo) || (pass == 1 && !is_diplo)) continue;
                    if (shown >= 7 || thought_y > 420) break;
                    int category_colour = 15;
                    if (thought.stage_near) {
                        category_colour = 7;
                    } else if (thought.category == "Economy") {
                        category_colour = 5;
                    } else if (thought.category == "Spoils") {
                        category_colour = 8;
                    } else if (thought.category == "Diplomacy") {
                        category_colour = 10;
                    }

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
        ren.SetScissor();
    }

    /** @brief A shield switches side; the engine button toggles the side's AI; Close closes. */
    void Mousepressed(int x, int y, int) override {
        w2::Game& game = Ctx().ActiveGame();
        for (int i = 0; i < 8; i++) {
            if (!Playing(game, i)) continue;
            kit::Rect shield{374 + i * 26, 41, 20, 20};
            if (shield.Contains(x, y)) {
                side_idx_ = i;
                return;
            }
        }
        if (kBtnToggleEngine.Contains(x, y)) {
            w2::Side& target_side = game.GetMap().Sides()[side_idx_];
            w2::ai::SetSideClassicAi(game, target_side, !target_side.IsClassicAi());
            return;
        }
        if (kBtnClose.Contains(x, y)) {
            PopSelf();
        }
    }

    /** @brief Left and Right switch to the previous or next side playing; F10 and Escape close. */
    void Keypressed(const std::string& key) override {
        w2::Game& game = Ctx().ActiveGame();
        if (key == "left" || key == "right") {
            int direction = (key == "right") ? 1 : 7;
            for (int step = 1; step <= 8; step++) {
                int candidate = (side_idx_ + direction * step) % 8;
                if (Playing(game, candidate)) {
                    side_idx_ = candidate;
                    break;
                }
            }
            return;
        }
        if (key == "f10" || key == "escape" || key == "return" || key == "kpenter") {
            PopSelf();
        }
    }

  private:
    static constexpr kit::Rect kBox{44, 34, 552, 412};
    static constexpr kit::Rect kBtnToggleEngine{380, 71, 128, 19};
    static constexpr kit::Rect kBtnClose{514, 71, 64, 19};

    int side_idx_ = 0;  ///< The side inspected.
};

}  // namespace

void Open(w2::UiContext& ctx) {
    if (ctx.ActiveGame().GetMap().GameOptions().diplomacy == 0) return;
    if (ctx.ModernUi()) {
        OpenReport(ctx);
    } else {
        OpenClassicReport(ctx);
    }
}

void Action(w2::UiContext& ctx) {
    if (ctx.ActiveGame().GetMap().GameOptions().diplomacy == 0) return;
    if (ctx.ModernUi()) {
        OpenAction(ctx);
    } else {
        OpenClassicAction(ctx);
    }
}

void OpenParley(w2::UiContext& ctx, int other_side, bool return_to_action) {
    if (!ctx.ModernUi() || !ctx.GamePtr() || other_side < 0 || other_side >= 8) return;
    kit::Push(ctx, std::make_shared<ParleyModal>(ctx, other_side, return_to_action));
}

void PollMissives(w2::UiContext& ctx, kit::Done after) {
    if (!ctx.ModernUi()) {
        if (ctx.Player()) ctx.Player()->ClearMissives();
        if (after) after();
        return;
    }
    if (!ctx.GamePtr() || !ctx.Player() || ctx.Player()->IsComputerControlled() ||
        ctx.Player()->Missives().empty()) {
        if (after) after();
        return;
    }
    w2::DiplomaticMissive missive = ctx.Player()->PopFrontMissive();
    if (!Playing(ctx.ActiveGame(), missive.from_side)) {
        PollMissives(ctx, std::move(after));
        return;
    }
    kit::Push(ctx, std::make_shared<MissiveModal>(ctx, std::move(missive), std::move(after)));
}

void OpenBrainInspector(w2::UiContext& ctx, int initial_side) {
    if (!ctx.ModernUi() || !ctx.GamePtr()) return;
    w2::Game& game = ctx.ActiveGame();
    int side_idx = 0;
    if (initial_side >= 0 && initial_side < 8 && Playing(game, initial_side)) {
        side_idx = initial_side;
    } else {
        for (int i = 0; i < 8; i++) {
            if (Playing(game, i) && game.GetMap().Sides()[i].IsComputerControlled()) {
                side_idx = i;
                break;
            }
        }
    }
    kit::Push(ctx, std::make_shared<BrainInspectorModal>(ctx, side_idx));
}

int ButtonFor(w2::Game& game, int side) {
    if (game.GetMap().GameOptions().diplomacy == 0) return w2::kNone;
    bool friendlier = false;
    bool hostile = false;
    for (int other = 0; other < 8; other++) {
        if (other != side) {
            int state = dip::State(game, other, side);
            int proposal = dip::Proposal(game, other, side);
            if (proposal != state) {
                if (proposal < state) {
                    friendlier = true;
                } else {
                    hostile = true;
                }
            } else if (dip::ProposalAllied(game, other, side) &&
                       !dip::IsAllied(game, other, side)) {
                friendlier = true;
            }
        }
    }
    if (!(friendlier || hostile)) return 183;
    return hostile ? 184 : 185;
}

}  // namespace diplomacyui
