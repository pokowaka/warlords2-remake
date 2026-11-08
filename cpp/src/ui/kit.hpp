// The pieces every dialog is made of, and the stack they are shown on.
//
// A popup (54f6:0000) is a black outline and a two-pixel shadow round a crop
// of MARBLE.PCK, text is drawn centred (7ecb:00d6) or from its left edge, an
// army by 8611:08be with a ring under it, a shield by 8611:0bf7, and a
// dialog's buttons are its BUTTON.DAT controls.
//
// A dialog is a Modal: draw() and, as it needs them, the input handlers.
// kit::push puts one on top; the front end sends input to the top one only.
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

struct Modal : std::enable_shared_from_this<Modal> {
    virtual ~Modal() = default;
    virtual void Draw() = 0;
    virtual void Update() {}
    virtual void Mousepressed(int x, int y, int button) {}
    /** True when the dialog took the release (else the screen's buttons do). */
    virtual bool Mousereleased(int x, int y, int button) { return false; }
    virtual void Keypressed(const std::string& key) {}
    virtual void Textinput(const std::string& text) {}
    /** A field is being typed into: the Steam Deck's on-screen keyboard comes up. */
    virtual bool Typing() const { return false; }
    /** The right button on the dialog, where no control's help answered. */
    virtual void Rightpressed(int fx, int fy, int sx, int sy) {}
    /** A help record's sub-id that stands for something on this dialog
     *  (infobox::control): true when a box went up. */
    virtual bool Info(int sub, int sx, int sy) { return false; }
    screen::View* view = nullptr;  // the controls the right button can ask about
    std::set<int> hidden;          // controls taken off the dialog's face
    bool menu_bar = false;         // the menu bar stays live over it
    bool infobox = false;
};
using ModalP = std::shared_ptr<Modal>;

ModalP Push(ModalP d);
void Pop(const Modal* d);
Modal* Top();
/** The top dialog, held: a handler may pop it while it runs. */
ModalP TopShared();

/** Set a palette colour by its index, 0-15. */
void SetPal(int i);
/** A one-pixel outline, x..x+w-1 by y..y+h-1, in the current colour. */
void Outline(double x, double y, double w, double h);
/** The frame 54f6:0000 gives a popup. */
void PopupFrame(const Rect& r);
/** A popup with no picture of its own: MARBLE.PCK cropped to the rect. */
void Popup(const Rect& r);
/** 78a8:06ae picks the font by number: 1 is CHANCE36, 2 CHANCE17, else TEXT. */
const Font& GetFont(int n);
/** Centred on x, with y the top of the line (7ecb:00d6). */
void Centred(const Font& f, const std::string& text, double x, double y);
/** Right-aligned, ending at x (7ecb:0103). */
void Right(const Font& f, const std::string& text, double x, double y);
/** A one-pixel bevel (24d0:02e5). */
void Bevel(double x, double y, double w, double h, int a, int b);
/** A text field (7ecb:0058). */
void Field(double x, double y, double w, double h, const std::string* text,
           const Font* f = nullptr);
inline void Field(double x, double y, double w, double h, const std::string& text,
                  const Font* f = nullptr) {
    Field(x, y, w, h, &text, f);
}
/** An army as 8611:08be draws one: the ring (0 none, 1 grey, 2-9 a side's),
 *  then the army's cell -- of ASHADOW.PCK when `shadow`. */
void Army(int type_id, int side, double x, double y, int ring = 0, bool shadow = false);
/** A side's big shield (8611:0bf7): SHIELDS.PCK's 40x40 cell. */
void Shield(int side, double x, double y);
/** A one-pixel line in palette colour c, end to end (Bresenham). */
void Line(int c, int x0, int y0, int x1, int y1);
/** The way to a place on a strategic map drawn at (mx, my) (828e:0a3f). */
void MapTarget(int mx, int my, int hx, int hy, int tx, int ty);
/** A dialog's BUTTON.DAT controls, each in its own state. */
screen::View View(int dialog_id);
void DrawControls(const screen::View& v, const std::set<int>* hidden = nullptr);
inline void DrawControls(const screen::View& v, const std::set<int>& hidden) {
    DrawControls(v, &hidden);
}
/** The live control under a point: not disabled and not hidden. */
const w2::uidata::Control* ControlAt(const screen::View& v, int x, int y,
                                     const std::set<int>* hidden = nullptr);
inline const w2::uidata::Control* ControlAt(const screen::View& v, int x, int y,
                                            const std::set<int>& hidden) {
    return ControlAt(v, x, y, &hidden);
}
const w2::uidata::Control* FindControl(const screen::View& v, int id);
/** A string from STRING.DAT, numbered as get_string numbers them. */
std::string Text(int group, int i);
/** An army sheet's cell for a type. */
gfx::Quad ArmyQuad(int type_id);
/** The time, in seconds. */
double Now();

}  // namespace kit
