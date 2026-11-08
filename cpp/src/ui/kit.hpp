// Shared building blocks for modal dialogs and the modal stack they live on.
//
// A popup is a black outline and a two-pixel drop shadow around a crop of
// MARBLE.PCK; text is drawn centred, right-aligned, or from its left edge;
// armies are drawn with optional selection rings; shields come from
// SHIELDS.PCK; and dialog buttons are driven by BUTTON.DAT records.
//
// Each dialog is a Modal with Draw() and optional input handlers. kit::Push
// places one on top of the modal stack, and the front end routes input to the
// top modal only.
#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "front/app.hpp"
#include "front/font.hpp"
#include "front/layout.hpp"
#include "front/screen.hpp"

namespace kit {

using layout::Rect;
using Done = std::function<void()>;

/**
 * @brief Base class for modal dialogs pushed onto the front-end modal stack.
 */
struct Modal : std::enable_shared_from_this<Modal> {
    virtual ~Modal() = default;
    /** @brief Renders the modal in the 640x480 dialog coordinate frame. */
    virtual void Draw() = 0;
    /** @brief Per-frame update hook while this modal is on top. */
    virtual void Update() {}
    /** @brief Handles a mouse button press at dialog coordinates `(x, y)`. */
    virtual void Mousepressed(int x, int y, int button) {}
    /** @brief Handles mouse button release; returns true when consumed by the modal. */
    virtual bool Mousereleased(int x, int y, int button) { return false; }
    /** @brief Handles a key press while this modal is on top. */
    virtual void Keypressed(const std::string& key) {}
    /** @brief Handles text input events while this modal is on top. */
    virtual void Textinput(const std::string& text) {}
    /** @brief True when a text field is active (triggers the Steam Deck on-screen keyboard). */
    virtual bool Typing() const { return false; }
    /** @brief Handles right-click at frame `(frame_x, frame_y)` / screen `(screen_x, screen_y)`. */
    virtual void Rightpressed(int frame_x, int frame_y, int screen_x, int screen_y) {}
    /** @brief Shows context help for `sub` at `(screen_x, screen_y)`; returns true if shown. */
    virtual bool Info(int sub, int screen_x, int screen_y) { return false; }
    screen::View* view = nullptr;  ///< Controls that right-click context help can inspect.
    std::set<int> hidden;          ///< Control IDs hidden from the dialog face.
    bool menu_bar = false;         ///< True when the top menu bar remains active over this modal.
    bool infobox = false;          ///< True when this modal is a transient hold-to-view infobox.
};
using ModalP = std::shared_ptr<Modal>;

/** @brief Pushes `modal` onto the top of the modal stack and returns it. */
ModalP Push(ModalP modal);
/** @brief Removes `modal` from the modal stack. */
void Pop(const Modal* modal);
/** @brief Returns a raw pointer to the top modal, or nullptr if the stack is empty. */
Modal* Top();
/** @brief Returns a shared pointer to the top modal so a handler may safely pop it. */
ModalP TopShared();

/** @brief Sets the active drawing colour to palette index `index` (0..15). */
void SetPal(int index);
/** @brief Draws a 1-pixel rectangle outline covering `x..x+width-1` by `y..y+height-1`. */
void Outline(double x, double y, double width, double height);
/** @brief Draws the black outline and drop shadow around `rect`. */
void PopupFrame(const Rect& rect);
/** @brief Draws a framed popup filled with `MARBLE.PCK` cropped to `rect`. */
void Popup(const Rect& rect);
/** @brief Returns a UI font by index: 1 is `CHANCE36`, 2 is `CHANCE17`, otherwise `TEXT`. */
const Font& GetFont(int n);
/** @brief Draws `text` horizontally centred on `x` with top at `y`. */
void Centred(const Font& font, const std::string& text, double x, double y);
/** @brief Draws `text` right-aligned ending at `x` with top at `y`. */
void Right(const Font& font, const std::string& text, double x, double y);
/** @brief Draws a 1-pixel bevelled border with top-left colour `top_col` and bottom-right
 * `bot_col`. */
void Bevel(double x, double y, double width, double height, int top_col, int bot_col);
/** @brief Draws a recessed text field box and optional `text` inside it. */
void Field(double x, double y, double width, double height, const std::string* text,
           const Font* font = nullptr);
/** @brief Draws a recessed text field box with `text` inside it. */
inline void Field(double x, double y, double width, double height, const std::string& text,
                  const Font* font = nullptr) {
    Field(x, y, width, height, &text, font);
}
/**
 * @brief Draws an army sprite with optional selection `ring` (0 none, 1 grey,
 * 2..9 side colour) or `ASHADOW.PCK` silhouette when `shadow` is true.
 */
void Army(int type_id, int side, double x, double y, int ring = 0, bool shadow = false);
/** @brief Draws a side's 40x40 shield from `SHIELDS.PCK`. */
void Shield(int side, double x, double y);
/** @brief Draws a 1-pixel Bresenham line in palette colour `colour_index`. */
void Line(int colour_index, int from_x, int from_y, int to_x, int to_y);
/** @brief Draws the line and target marker on a strategic map at `(map_x, map_y)`. */
void MapTarget(int map_x, int map_y, int hero_x, int hero_y, int target_x, int target_y);
/** @brief Creates a `screen::View` for `dialog_id` with its controls in their initial states. */
screen::View View(int dialog_id);
/** @brief Draws all visible controls in `view`, skipping any IDs in `hidden`. */
void DrawControls(const screen::View& view, const std::set<int>* hidden = nullptr);
/** @brief Draws all visible controls in `view`, skipping any IDs in `hidden`. */
inline void DrawControls(const screen::View& view, const std::set<int>& hidden) {
    DrawControls(view, &hidden);
}
/** @brief Returns the enabled, unhidden control in `view` under `(x, y)`, or nullptr. */
const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                     const std::set<int>* hidden = nullptr);
/** @brief Returns the enabled, unhidden control in `view` under `(x, y)`, or nullptr. */
inline const w2::uidata::Control* ControlAt(const screen::View& view, int x, int y,
                                            const std::set<int>& hidden) {
    return ControlAt(view, x, y, &hidden);
}
/** @brief Looks up control `control_id` in `view`. */
const w2::uidata::Control* FindControl(const screen::View& view, int control_id);
/** @brief Returns string `index` from `STRING.DAT` group `group`. */
std::string Text(int group, int index);
/** @brief Returns the sprite sheet quad for `type_id`. */
gfx::Quad ArmyQuad(int type_id);
/** @brief Returns elapsed monotonic time in seconds. */
double Now();

}  // namespace kit
