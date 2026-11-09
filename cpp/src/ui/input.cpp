/**
 * @file
 * @brief Single-line text-entry and confirmation dialog (`input::Open` and `input::Editor`).
 *
 * Popup 1, `(160, 90)` `320x200`: the title in font 1 centred on `x = 320` at
 * `y = 92`, the prompt in font 2, and the field at `(184, 197)` `256x20` over
 * dialog 5: OK (`189`), Cancel (`190`) and the field's own hit area (`191`).
 */
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/dialogs.hpp"

namespace input {

namespace {

using RowYList = std::vector<int>;
using OptionalEditor = std::optional<Editor>;

constexpr kit::Rect kPopupRect{160, 90, 320, 200};  // popup 1
constexpr int kDialog = 5;
constexpr int kOk = 189;
constexpr int kCancel = 190;
constexpr int kField = 191;
constexpr int kTitleY = 92;
constexpr kit::Rect kBox{184, 197, 256, 20};
constexpr double kBlink = 0.25;

/**
 * @brief Returns prompt row y-positions for `line_count` total lines.
 *
 * The text-entry form counts two extra lines for the input field.
 */
RowYList PromptRowYs(int line_count) {
    switch (line_count) {
    case 1:
        return {150};
    case 2:
        return {150, 173};
    case 3:
        return {150, 173, 196};
    case 4:
        return {140, 163, 186, 209};
    default:
        return {};
    }
}

/**
 * @brief Modal implementation for text input and confirmation prompts.
 */
class Dialog final : public kit::Modal {
  public:
    Dialog(w2::UiContext& ctx, const Options& opts)
            : kit::Modal(ctx), opts_(opts), view_(kit::View(ctx, kDialog)), value_(opts.text) {
        view_.state[kOk] = w2::uidata::kNormal;
        view_.state[kCancel] = opts.cancel ? w2::uidata::kNormal : w2::uidata::kDisabled;
        SetDialogView(&view_);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        kit::Popup(ctx, kPopupRect);
        ctx.Renderer().SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), opts_.title, 320, kTitleY);
        int line_count = static_cast<int>(opts_.lines.size()) + (opts_.confirm ? 0 : 2);
        RowYList row_ys = PromptRowYs(line_count);
        for (size_t i = 0; i < opts_.lines.size() && i < row_ys.size(); i++) {
            kit::Centred(kit::GetFont(ctx, 2), opts_.lines[i], 320, row_ys[i]);
        }
        if (opts_.confirm) {
            kit::ControlIdSet hidden_ids{kField};
            kit::DrawControls(ctx, view_, hidden_ids);
            return;
        }
        kit::Field(ctx, kBox.x, kBox.y, kBox.w, kBox.h, editing_ ? editing_->text : value_,
                   &kit::GetFont(ctx, 2));
        if (editing_) editing_->DrawCursor(ctx, kBox.x, kBox.y);
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int) override {
        if (editing_) {
            value_ = editing_->text;
            editing_.reset();
        }
        kit::ControlIdSet hidden_ids;
        if (opts_.confirm) hidden_ids.insert(kField);
        const auto* control = kit::ControlAt(view_, x, y, hidden_ids);
        if (!control) return;
        if (control->id == kOk) {
            Finish(true);
        } else if (control->id == kCancel) {
            Finish(false);
        } else if (control->id == kField) {
            editing_ = Editor();
            editing_->max_chars = opts_.max_chars;
            editing_->max_width = opts_.max_width;
        }
    }

    void Keypressed(const std::string& key) override {
        if (editing_) {
            std::string action = editing_->Key(key);
            if (action == "keep") {
                value_ = editing_->text;
                editing_.reset();
            } else if (action == "undo") {
                editing_.reset();
            }
            return;
        }
        if (key == "return" || key == "kpenter") {
            Finish(true);
        } else if (key == "escape" && view_.StateOf(kCancel) != w2::uidata::kDisabled) {
            Finish(false);
        }
    }

    void Textinput(const std::string& typed) override {
        if (editing_) editing_->Input(Ctx(), typed);
    }

    bool Typing() const override { return editing_.has_value(); }

  private:
    void Finish(bool confirmed) {
        Options options = opts_;
        std::string text_value = value_;
        PopSelf();
        if (confirmed) {
            if (options.ok) options.ok(text_value);
        } else if (options.cancelled) {
            options.cancelled();
        }
    }

    Options opts_;            ///< Dialog options and callbacks.
    screen::View view_;       ///< Dialog 5 control states.
    std::string value_;       ///< Committed text value.
    OptionalEditor editing_;  ///< Active editor while typing into the field.
};

}  // namespace

std::string Editor::Key(const std::string& key) {
    if (key == "return" || key == "kpenter") return "keep";
    if (key == "escape") return "undo";
    if (key == "backspace" && !text.empty()) text.pop_back();
    return "";
}

void Editor::Input(const w2::UiContext& ctx, const std::string& typed) {
    for (unsigned char byte : typed) {
        if (byte >= 0x20 && byte < 0x7b && static_cast<int>(text.size()) < max_chars - 1 &&
            kit::GetFont(ctx, 2).Width(text) < max_width) {
            text += static_cast<char>(byte);
        }
    }
}

void Editor::DrawCursor(w2::UiContext& ctx, double x, double y) const {
    const Font& font = kit::GetFont(ctx, 2);
    if (static_cast<long>(std::floor(kit::Now() / kBlink)) % 2 == 0) {
        kit::SetPal(ctx, 9);
    } else {
        ctx.Renderer().SetColor(1, 1, 1);
    }
    font.Draw("`", x + font.Width(text) + 5, y + 2);
}

void Open(w2::UiContext& ctx, const Options& opts) {
    kit::Push(ctx, std::make_shared<Dialog>(ctx, opts));
}

}  // namespace input
