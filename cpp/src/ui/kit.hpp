/**
 * @file
 * @brief Shared building blocks for modal dialogs (`kit::Modal`) and `MARBLE.PCK` UI primitives.
 *
 * A popup is a black outline and a two-pixel drop shadow around a crop of `MARBLE.PCK`; text is
 * drawn centred, right-aligned, or from its left edge; armies are drawn with optional selection
 * rings; shields come from `SHIELDS.PCK`; and dialog buttons are driven by `BUTTON.DAT` records.
 */
#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "front/font.hpp"
#include "front/layout.hpp"
#include "front/screen.hpp"
#include "ui/ui_context.hpp"

namespace kit {

using layout::Rect;
using Done = std::function<void()>;
using ControlIdSet = std::set<int>;

/**
 * @brief Polymorphic base class for modal dialogs pushed onto `w2::ModalStack`.
 *
 * Holds a non-owning `w2::UiContext&` (`Ctx()`) injected at construction time along with
 * right-click control-help metadata (`DialogView()`, `HiddenControls()`), menu-bar passthrough,
 * transient infobox, and turn-opening flags.
 *
 * Thread safety: Subclasses execute exclusively on the main UI thread.
 */
class Modal : public std::enable_shared_from_this<Modal> {
  public:
    explicit Modal(w2::UiContext& ctx) : ctx_(ctx) {}
    virtual ~Modal() = default;

    Modal(const Modal&) = delete;
    Modal& operator=(const Modal&) = delete;

    /** @brief Renders the modal in the 640x480 dialog coordinate frame. */
    virtual void Draw() = 0;
    /** @brief Per-frame update hook while this modal is on top. */
    virtual void Update() {}
    /** @brief Handles a mouse button press at dialog coordinates `(x, y)`. */
    virtual void Mousepressed(int x, int y, int button) {
        (void)x;
        (void)y;
        (void)button;
    }
    /** @brief Handles mouse button release; returns true when consumed by the modal. */
    virtual bool Mousereleased(int x, int y, int button) {
        (void)x;
        (void)y;
        (void)button;
        return false;
    }
    /** @brief Handles a key press while this modal is on top. */
    virtual void Keypressed(const std::string& key) { (void)key; }
    /** @brief Handles text input events while this modal is on top. */
    virtual void Textinput(const std::string& text) { (void)text; }
    /** @brief True when a text field is active (triggers the Steam Deck on-screen keyboard). */
    virtual bool Typing() const { return false; }
    /** @brief Returns SDL typing state: `0` off, `1` hardware keys only, `2` on-screen keyboard. */
    virtual int TypingState() const { return Typing() ? 2 : 0; }
    /** @brief Handles right-click at frame `(frame_x, frame_y)` / screen `(screen_x, screen_y)`. */
    virtual void Rightpressed(int frame_x, int frame_y, int screen_x, int screen_y) {
        (void)frame_x;
        (void)frame_y;
        (void)screen_x;
        (void)screen_y;
    }
    /** @brief Shows context help for `sub` at `(screen_x, screen_y)`; returns true if shown. */
    virtual bool Info(int sub, int screen_x, int screen_y) {
        (void)sub;
        (void)screen_x;
        (void)screen_y;
        return false;
    }

    /** @brief Returns the injected UI dependency context. */
    w2::UiContext& Ctx() const { return ctx_; }

    /** @brief Pops this modal from `ctx_.Modals()`. */
    void PopSelf();

    screen::View* DialogView() const { return view_; }
    void SetDialogView(screen::View* view) { view_ = view; }

    ControlIdSet& HiddenControls() { return hidden_; }
    const ControlIdSet& HiddenControls() const { return hidden_; }

    bool UsesMenuBar() const { return menu_bar_; }
    void SetUsesMenuBar(bool enabled) { menu_bar_ = enabled; }

    bool IsInfobox() const { return infobox_; }
    void SetIsInfobox(bool enabled) { infobox_ = enabled; }

    bool GreysMainControls() const { return greys_main_controls_; }
    void SetGreysMainControls(bool enabled) { greys_main_controls_ = enabled; }

    bool IsTurnBanner() const { return is_turn_banner_; }
    void SetIsTurnBanner(bool enabled) { is_turn_banner_ = enabled; }

    bool IsHeroOffer() const { return is_hero_offer_; }
    void SetIsHeroOffer(bool enabled) { is_hero_offer_ = enabled; }

    bool IsCityVictory() const { return is_city_victory_; }
    void SetIsCityVictory(bool enabled) { is_city_victory_ = enabled; }

  private:
    w2::UiContext& ctx_;            ///< Injected non-owning UI context.
    screen::View* view_ = nullptr;  ///< Optional dialog `View` for right-click control help.
    ControlIdSet hidden_;           ///< Control IDs hidden from `view_`.
    bool menu_bar_ = false;  ///< True when the top menu bar stays interactive above this modal.
    bool infobox_ = false;   ///< True when this modal is a hold-to-view right-click infobox.
    bool greys_main_controls_ = false;  ///< True when main-screen buttons stay disabled underneath.
    bool is_turn_banner_ = false;       ///< True for `TurnBannerModal`.
    bool is_hero_offer_ = false;        ///< True for `HeroOfferModal`.
    bool is_city_victory_ = false;      ///< True for `CityVictoryModal`.
};

using ModalP = std::shared_ptr<Modal>;

/** @brief Pushes `modal` onto `ctx.Modals()` and returns it. */
ModalP Push(w2::UiContext& ctx, ModalP modal);
/** @brief Removes `modal` from `ctx.Modals()`. */
void Pop(w2::UiContext& ctx, const Modal* modal);
/** @brief Returns a raw pointer to the top modal on `ctx.Modals()`, or `nullptr`. */
Modal* Top(const w2::UiContext& ctx);
/** @brief Returns a shared pointer to the top modal on `ctx.Modals()`, or `nullptr`. */
ModalP TopShared(const w2::UiContext& ctx);

/** @brief Sets `ctx.Renderer()`'s active colour to palette index `index` (`0..15`). */
void SetPal(w2::UiContext& ctx, int index);
/** @brief Draws a 1-pixel rectangle outline covering `x..x+width-1` by `y..y+height-1`. */
void Outline(w2::UiContext& ctx, double x, double y, double width, double height);
/** @brief Draws the black outline and drop shadow around `rect`. */
void PopupFrame(w2::UiContext& ctx, const Rect& rect);
/** @brief Draws a framed popup filled with `MARBLE.PCK` cropped to `rect`. */
void Popup(w2::UiContext& ctx, const Rect& rect);
/** @brief Returns a UI font by index: `1` is `CHANCE36`, `2` is `CHANCE17`, otherwise `TEXT`. */
const Font& GetFont(const w2::UiContext& ctx, int n);
/** @brief Draws `text` horizontally centred on `x` with top at `y`. */
void Centred(const Font& font, const std::string& text, double x, double y);
/** @brief Draws `text` right-aligned ending at `x` with top at `y`. */
void Right(const Font& font, const std::string& text, double x, double y);
/** @brief Draws a 1-pixel bevelled border with top-left `top_col` and bottom-right `bot_col`. */
void Bevel(w2::UiContext& ctx, double x, double y, double width, double height, int top_col,
           int bot_col);
/** @brief Draws a recessed text field box and optional `text` inside it. */
void Field(w2::UiContext& ctx, double x, double y, double width, double height,
           const std::string* text, const Font* font = nullptr);
/** @brief Draws a recessed text field box with `text` inside it. */
inline void Field(w2::UiContext& ctx, double x, double y, double width, double height,
                  const std::string& text, const Font* font = nullptr) {
    Field(ctx, x, y, width, height, &text, font);
}
/**
 * @brief Draws an army sprite with optional selection `ring` (`0` none, `1` grey, `2..9` side
 * colour) or `ASHADOW.PCK` silhouette when `shadow` is true.
 */
void Army(w2::UiContext& ctx, int type_id, int side, double x, double y, int ring = 0,
          bool shadow = false);
/** @brief Draws a side's 40x40 shield from `SHIELDS.PCK`. */
void Shield(w2::UiContext& ctx, int side, double x, double y);
/** @brief Draws a 1-pixel Bresenham line in palette colour `colour_index`. */
void Line(w2::UiContext& ctx, int colour_index, int from_x, int from_y, int to_x, int to_y);
/** @brief Draws the line and target marker on a strategic map at `(map_x, map_y)`. */
void MapTarget(w2::UiContext& ctx, int map_x, int map_y, int hero_x, int hero_y, int target_x,
               int target_y);
/** @brief Creates a `screen::View` for `dialog_id` with its controls in their initial states. */
screen::View View(w2::UiContext& ctx, int dialog_id);
/** @brief Draws all visible controls in `view`, skipping any IDs in `hidden`. */
void DrawControls(w2::UiContext& ctx, const screen::View& view,
                  const ControlIdSet* hidden = nullptr);
/** @brief Draws all visible controls in `view`, skipping any IDs in `hidden`. */
inline void DrawControls(w2::UiContext& ctx, const screen::View& view, const ControlIdSet& hidden) {
    DrawControls(ctx, view, &hidden);
}
/** @brief Returns the enabled, unhidden control in `view` under `(x, y)`, or `nullptr`. */
const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const ControlIdSet* hidden = nullptr);
/** @brief Returns the enabled, unhidden control in `view` under `(x, y)`, or `nullptr`. */
inline const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                            const ControlIdSet& hidden) {
    return ControlAt(view, x, y, &hidden);
}
/** @brief Looks up control `control_id` in `view`. */
const w2::uidata::Control* FindControl(const screen::View& view, int control_id);
/** @brief Returns string `index` from `STRING.DAT` group `group`. */
std::string Text(const w2::UiContext& ctx, int group, int index);
/** @brief Returns the sprite sheet quad for `type_id`. */
gfx::Quad ArmyQuad(int type_id);
/** @brief Returns elapsed monotonic time in seconds. */
double Now();

}  // namespace kit
