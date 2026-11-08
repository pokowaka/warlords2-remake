// Report > Diplomacy (484e:0000): the Diplomatic Report, Diplomatic Action,
// Parley & Attitude Ledger, Turn-Start Envoy Missives, and F10 AI Brain Inspector.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "ui/dialogs.hpp"
#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"

namespace diplomacyui {

namespace dip = w2::diplomacy;

namespace {
const kit::Rect R{80, 60, 480, 350};  // popup 11
const int REPORT = 21, DONE = 368, ACTION = 369;
const int ACT = 22, OK = 370, BACK = 371;
const int OFFER[3] = {372, 380, 388};  // a row of 7 each

bool playing(const w2::Game& g, int i) {
    return i >= 0 && i < 8 && g.map->sides[i].inUse && g.map->sides[i].alive;
}

int activeViewerIndex(const w2::Game& g) {
    if (G.player && G.player->index >= 0 && G.player->index < 8) return G.player->index;
    for (int i = 0; i < 8; i++) {
        if (playing(g, i)) return i;
    }
    return 0;
}

void diplom(int sx, int sy, int w, int h, int x, int y) {
    if (auto art = G.screen->artFor(47)) {
        gfx::setColor(1, 1, 1);
        gfx::draw(art->image, gfx::newQuad(sx, sy, w, h), x, y);
    }
}

void smallShield(int side, int x, int y) {
    gfx::setColor(1, 1, 1);
    gfx::draw(G.shieldsImg, gfx::newQuad(side * 40 + 24, 46, 16, 16), x, y);
}

void blank(int x, int y) {
    kit::setPal(0);
    kit::outline(x, y, 40, 40);
    kit::setPal(2);
    gfx::rectangle(gfx::FILL, x + 1, y + 1, 38, 38);
}

// ---------------------------------------------------------------------------
// 1993 Classic Diplomacy UI (default unless --modern-ui is enabled)
// ---------------------------------------------------------------------------

void openClassicAction();

struct ClassicReport : kit::Modal {
    screen::View v;
    std::map<int, std::string> titles;
    std::vector<w2::Side*> order;
    void draw() override {
        w2::Game& g = *G.g;
        kit::popup(R);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6b, 0), 320, 64);
        kit::setPal(1);
        gfx::rectangle(gfx::FILL, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 120 + 32 * i, 110, 1, 261);
        gfx::rectangle(gfx::FILL, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (playing(g, i)) {
                smallShield(i, 128 + 32 * i, 117);
                smallShield(i, 96, 146 + 29 * i);
            }
        }
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    diplom(384, 29, 32, 29, x, y);
                else if (playing(g, col) && playing(g, row)) {
                    int st = dip::state(g, col, row);
                    if (st == dip::WAR)
                        diplom(384, 0, 32, 29, x, y);
                    else if (st == dip::PEACE)
                        diplom(320, 0, 32, 29, x, y);
                }
            }
        }
        kit::setPal(0);
        kit::outline(392, 110, 160, 262);
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        kit::centred(f, kit::text(0x6d, 0), 472, 116);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * (int)i;
            smallShield(order[i]->index, 400, y);
            gfx::setColor(1, 1, 1);
            f.draw(titles[order[i]->index], 432, y);
        }
        kit::drawControls(v, hidden);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y, hidden);
        if (!c) return;
        if (c->id == DONE)
            kit::pop(this);
        else if (c->id == ACTION) {
            kit::pop(this);
            openClassicAction();
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

void openClassicReport() {
    auto d = std::make_shared<ClassicReport>();
    d->v = kit::view(REPORT);
    d->view = &d->v;
    d->v.state[DONE] = d->v.state[ACTION] = w2::uidata::NORMAL;
    if (!w2::game::hasActiveHuman(*G.g)) d->hidden.insert(ACTION);
    d->titles = dip::ratings(*G.g);
    for (w2::Side* s : G.g->sides)
        if (s->alive) d->order.push_back(s);
    w2::luaSort(d->order, [](w2::Side* p, w2::Side* q) {
        if (p->diploScore != q->diploScore) return p->diploScore < q->diploScore;
        return p->index < q->index;
    });
    kit::push(d);
}

struct ClassicAction : kit::Modal {
    screen::View v;
    std::vector<int> others;
    void draw() override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        const w2::Side& mySide = g.map->sides[me];
        kit::popup(R);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6c, 0), 320, 64);
        kit::shield(me, 96, 111);
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        f.draw(mySide.name, 144, 122);
        const int ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) f.draw(kit::text(0x6e, i), 104, ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * (int)col;
            kit::shield(other, x, 111);
            if (!playing(g, other)) {
                for (int y : {151, 191, 245, 285, 325}) blank(x, y);
            } else {
                int st = dip::state(g, other, me);
                diplom(80 + 120 * st, 45, 40, 40, x, 151);
                int theirs = dip::proposal(g, other, me);
                if (theirs == st)
                    blank(x, 191);
                else
                    diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                int mine = dip::proposal(g, me, other);
                for (int k = 0; k <= 2; k++)
                    diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
            }
        }
        std::set<int> h;
        for (int k = 0; k <= 2; k++)
            for (int i = 0; i <= 6; i++) h.insert(OFFER[k] + i);
        kit::drawControls(v, h);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == OK) {
            kit::pop(this);
            return;
        }
        if (c->id == BACK) {
            kit::pop(this);
            openClassicReport();
            return;
        }
        int me = activeViewerIndex(*G.g);
        for (int k = 0; k <= 2; k++) {
            int i = c->id - OFFER[k];
            if (i >= 0 && i < 7 && i < (int)others.size()) dip::propose(*G.g, me, others[i], k);
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

void openClassicAction() {
    auto d = std::make_shared<ClassicAction>();
    d->v = kit::view(ACT);
    d->view = &d->v;
    for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::NORMAL;
    int me = activeViewerIndex(*G.g);
    for (int i = 0; i < 8; i++)
        if (i != me) d->others.push_back(i);
    kit::push(d);
}

// Sanitize UTF-8 punctuation and bitmap-font repurposed glyphs:
// In SSG's TEXT.FNT (smallFont == true), ASCII '&' (38) is the menu checkmark
// glyph and '`' (96) is a solid block.
std::string cleanText(const std::string& raw, bool smallFont = true) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned char c = (unsigned char)raw[i];
        // UTF-8 em-dash / en-dash (E2 80 93 / E2 80 94) and curly quotes
        if (c == 0xe2 && i + 2 < raw.size() && (unsigned char)raw[i + 1] == 0x80) {
            unsigned char c2 = (unsigned char)raw[i + 2];
            if (c2 == 0x93 || c2 == 0x94) {
                out += '-';
                i += 2;
                continue;
            }
            if (c2 == 0x98 || c2 == 0x99) {
                out += '\'';
                i += 2;
                continue;
            }
            if (c2 == 0x9c || c2 == 0x9d) {
                out += '"';
                i += 2;
                continue;
            }
        }
        if (c < 32 || c >= 127) continue;
        if (smallFont && c == '&') {
            if (!out.empty() && out.back() == ' ' && i + 1 < raw.size() && raw[i + 1] == ' ') {
                out += "and";
            } else {
                out += '+';
            }
            continue;
        }
        if (c == '`') {
            out += '\'';
            continue;
        }
        out += (char)c;
    }
    return out;
}

std::string fitText(const Font& f, const std::string& raw, int maxW, bool smallFont = true) {
    std::string s = cleanText(raw, smallFont);
    if (maxW <= 0) return "";
    if (f.width(s) <= maxW) return s;
    const std::string ell = "..";
    int ellW = f.width(ell);
    if (ellW >= maxW) return "";
    while (!s.empty() && f.width(s) + ellW > maxW) {
        s.pop_back();
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + ell;
}

std::vector<std::string> wrapText(const Font& f, const std::string& raw, int maxW,
                                  bool smallFont = true) {
    std::string cleaned = cleanText(raw, smallFont);
    std::vector<std::string> lines;
    if (maxW <= 0 || cleaned.empty()) return lines;
    std::istringstream iss(cleaned);
    std::string word, cur;
    while (iss >> word) {
        if (f.width(word) > maxW) {
            word = fitText(f, word, maxW, false);
        }
        std::string test = cur.empty() ? word : cur + " " + word;
        if (f.width(test) <= maxW || cur.empty()) {
            cur = test;
        } else {
            lines.push_back(cur);
            cur = word;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Recessed dark-slate Warlords II information well with optional gold/cyan header strip.
void drawSunkenPanel(int x, int y, int w, int h, const std::string& leftHdr = "",
                     const std::string& rightHdr = "", int rightPal = 5) {
    kit::setPal(4);  // deep charcoal interior (31,31,31)
    gfx::rectangle(gfx::FILL, x, y, w, h);
    kit::bevel(x, y, w, h, 0, 1);  // sunken 3D stone bevel: black top/left, silver bottom/right
    if (!leftHdr.empty() || !rightHdr.empty()) {
        kit::setPal(3);  // subtle slate header band (44,44,44)
        gfx::rectangle(gfx::FILL, x + 1, y + 1, w - 2, 17);
        kit::setPal(0);
        gfx::rectangle(gfx::FILL, x + 1, y + 18, w - 2, 1);
        const Font& fSm = kit::font(0);
        gfx::setColor(1, 1, 1);
        int availW = w - 14;
        std::string rClean = rightHdr.empty() ? "" : fitText(fSm, rightHdr, availW / 2 + 36);
        int rW = rClean.empty() ? 0 : fSm.width(rClean) + 10;
        if (!leftHdr.empty()) {
            std::string lClean = fitText(fSm, leftHdr, std::max(24, availW - rW));
            fSm.colours(7, 0).draw(lClean, x + 7, y + 3);
        }
        if (!rClean.empty()) {
            kit::right(fSm.colours(rightPal, 0), rClean, x + w - 7, y + 3);
        }
    }
}

// Raised 3D stone button with crisp black-outlined text and semantic active fill.
void drawButton(int x, int y, int w, int h, const std::string& label, bool active = false,
                bool disabled = false, int accentPal = 11) {
    kit::setPal(0);
    kit::outline(x, y, w, h);
    kit::setPal(disabled ? 3 : (active ? accentPal : 2));
    gfx::rectangle(gfx::FILL, x + 1, y + 1, w - 2, h - 2);
    kit::bevel(x + 1, y + 1, w - 2, h - 2, active ? 7 : (disabled ? 2 : 1), 0);

    const Font& fSm = kit::font(0);
    int glyph = disabled ? 2 : (active ? 7 : 15);
    const Font& f = fSm.colours(glyph, 0);
    std::string fitted = fitText(fSm, label, w - 8);
    gfx::setColor(1, 1, 1);
    kit::centred(f, fitted, x + w / 2, y + (h - f.lineHeight) / 2);
}

// High-contrast heraldic font for dark slate panels (lifts Lich King black/navy to crisp readable
// tones).
const Font& sideBannerFont(const Font& fBig, const w2::Side& s) {
    if (s.colour == 0) return fBig.colours(15, s.edge == 0 ? 9 : s.edge);
    if (s.colour == 6) return fBig.colours(5, 0);
    return fBig.colours(s.colour, s.edge);
}

void openReport();
void openAction();

struct Report : kit::Modal {
    screen::View v;
    std::map<int, std::string> titles;
    std::vector<w2::Side*> order;
    void draw() override {
        w2::Game& g = *G.g;
        kit::popup(R);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6b, 0), 320, 64);
        kit::setPal(1);
        gfx::rectangle(gfx::FILL, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 120 + 32 * i, 110, 1, 261);
        gfx::rectangle(gfx::FILL, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (playing(g, i)) {
                smallShield(i, 128 + 32 * i, 117);
                smallShield(i, 96, 146 + 29 * i);
            }
        }
        const Font& tf = kit::font(0);
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    diplom(384, 29, 32, 29, x, y);
                else if (playing(g, col) && playing(g, row)) {
                    int st = dip::state(g, col, row);
                    if (st == dip::WAR)
                        diplom(384, 0, 32, 29, x, y);
                    else if (st == dip::PEACE) {
                        diplom(320, 0, 32, 29, x, y);
                        if (dip::isAllied(g, col, row)) {
                            kit::setPal(10);
                            kit::outline(x + 1, y + 1, 30, 27);
                            gfx::setColor(1, 1, 1);
                            tf.colours(7, 0).draw("ALLY", x + 3, y + 2);
                        }
                    }
                }
            }
        }
        // Recessed ratings panel on the right
        kit::setPal(4);
        gfx::rectangle(gfx::FILL, 392, 110, 160, 262);
        kit::bevel(392, 110, 160, 262, 0, 1);
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        kit::centred(f.colours(7, 0), kit::text(0x6d, 0), 472, 116);
        kit::setPal(0);
        gfx::rectangle(gfx::FILL, 394, 138, 156, 1);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * (int)i;
            smallShield(order[i]->index, 400, y);
            gfx::setColor(1, 1, 1);
            std::string rTitle = fitText(f, titles[order[i]->index], 120, false);
            f.draw(rTitle, 424, y);
        }
        // Centered cleanly between ACTION (96..160) and DONE (480..544)
        gfx::setColor(1, 1, 1);
        kit::centred(tf.colours(7, 0), "Click any shield to Parley  -  F10: AI Brain", 320, 384);
        kit::drawControls(v);
    }
    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        for (int i = 0; i < 8; i++) {
            if (i == me || !playing(g, i)) continue;
            kit::Rect topSh{120 + 32 * i, 110, 32, 28};
            kit::Rect leftSh{88, 139 + 29 * i, 32, 29};
            if (topSh.contains(x, y) || leftSh.contains(x, y)) {
                kit::pop(this);
                openParley(i, false);
                return;
            }
        }
        for (size_t i = 0; i < order.size(); i++) {
            int sIdx = order[i]->index;
            if (sIdx == me) continue;
            kit::Rect rateRow{394, 142 + 29 * (int)i, 156, 26};
            if (rateRow.contains(x, y)) {
                kit::pop(this);
                openParley(sIdx, false);
                return;
            }
        }
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == DONE)
            kit::pop(this);
        else if (c->id == ACTION) {
            kit::pop(this);
            openAction();
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "f10") {
            openBrainInspector();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

void openReport() {
    auto d = std::make_shared<Report>();
    d->v = kit::view(REPORT);
    d->view = &d->v;
    d->v.state[DONE] = d->v.state[ACTION] = w2::uidata::NORMAL;
    d->titles = dip::ratings(*G.g);
    for (w2::Side* s : G.g->sides)
        if (s->alive) d->order.push_back(s);
    w2::luaSort(d->order, [](w2::Side* p, w2::Side* q) {
        if (p->diploScore != q->diploScore) return p->diploScore < q->diploScore;
        return p->index < q->index;
    });
    kit::push(d);
}

struct Action : kit::Modal {
    screen::View v;
    std::vector<int> others;
    void draw() override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        const w2::Side& mySide = g.map->sides[me];
        kit::popup(R);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6c, 0), 320, 64);
        kit::shield(me, 96, 111);
        const Font& f = kit::font(2);
        const Font& tf = kit::font(0);
        gfx::setColor(1, 1, 1);
        sideBannerFont(f, mySide).draw(fitText(f, mySide.name, 122, false), 144, 122);
        const int ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) f.draw(kit::text(0x6e, i), 104, ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * (int)col;
            kit::shield(other, x, 111);
            if (!playing(g, other)) {
                for (int y : {151, 191, 245, 285, 325}) blank(x, y);
            } else {
                int st = dip::state(g, other, me);
                diplom(80 + 120 * st, 45, 40, 40, x, 151);
                if (dip::isAllied(g, other, me)) {
                    kit::setPal(10);
                    kit::outline(x + 1, 152, 38, 38);
                    gfx::setColor(1, 1, 1);
                    tf.colours(7, 0).draw("ALLY", x + 6, 153);
                }
                int tr = dip::truceTurns(g, me, other);
                if (tr > 0) {
                    gfx::setColor(1, 1, 1);
                    tf.colours(7, 0).draw(w2::fmt("T:%d", tr), x + 8, 177);
                }

                int theirs = dip::proposal(g, other, me);
                bool theirAlly = dip::proposalAllied(g, other, me);
                if (theirs == st && (!theirAlly || dip::isAllied(g, other, me)))
                    blank(x, 191);
                else {
                    diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                    if (theirAlly) {
                        gfx::setColor(1, 1, 1);
                        tf.colours(7, 0).draw("ALLY", x + 6, 193);
                    }
                }

                int mine = dip::proposal(g, me, other);
                bool myAlly = dip::proposalAllied(g, me, other);
                for (int k = 0; k <= 2; k++) {
                    diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
                }
                if (myAlly && mine == dip::PEACE) {
                    kit::setPal(10);
                    kit::outline(x + 1, 246, 38, 38);
                    gfx::setColor(1, 1, 1);
                    tf.colours(7, 0).draw("ALLY", x + 6, 247);
                }
                int gOff = dip::goldOffer(g, me, other);
                if (gOff > 0) {
                    gfx::setColor(1, 1, 1);
                    tf.colours(7, 0).draw(w2::fmt("+%dg", gOff), x + 3, 271);
                }
            }
        }
        // Centered cleanly between BACK (96..160) and OK (480..544)
        gfx::setColor(1, 1, 1);
        kit::centred(tf.colours(7, 0), "Click a top shield to Parley  -  F10: AI Brain", 320, 384);
        std::set<int> h;
        for (int k = 0; k <= 2; k++)
            for (int i = 0; i <= 6; i++) h.insert(OFFER[k] + i);
        kit::drawControls(v, h);
    }
    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            if (!playing(g, other)) continue;
            kit::Rect shRect{272 + 40 * (int)col, 108, 40, 43};
            if (shRect.contains(x, y)) {
                kit::pop(this);
                openParley(other, true);
                return;
            }
        }
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == OK) {
            kit::pop(this);
            return;
        }
        if (c->id == BACK) {
            kit::pop(this);
            openReport();
            return;
        }
        for (int k = 0; k <= 2; k++) {
            int i = c->id - OFFER[k];
            if (i >= 0 && i < 7 && i < (int)others.size()) {
                int other = others[i];
                if (k == dip::PEACE && dip::proposal(g, me, other) == dip::PEACE) {
                    bool nextAlly = !dip::proposalAllied(g, me, other);
                    dip::proposeAlliance(g, me, other, nextAlly);
                } else {
                    dip::propose(g, me, other, k);
                }
            }
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "f10") {
            openBrainInspector();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

void openAction() {
    auto d = std::make_shared<Action>();
    d->v = kit::view(ACT);
    d->view = &d->v;
    int me = activeViewerIndex(*G.g);
    for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::NORMAL;
    for (int i = 0; i < 8; i++)
        if (i != me) d->others.push_back(i);
    kit::push(d);
}

// ------------------------------------------------------------------ Parley & Attitude Ledger
struct ParleyModal : kit::Modal {
    int other = 0;
    bool returnToAction = false;

    const kit::Rect box{60, 44, 520, 384};
    const kit::Rect btnAlly{387, 166, 174, 21};
    const kit::Rect btnPeace{387, 189, 174, 21};
    const kit::Rect btnUneasy{387, 212, 174, 21};
    const kit::Rect btnWar{387, 235, 174, 21};
    const kit::Rect btnGoldMinus{387, 260, 84, 21};
    const kit::Rect btnGoldPlus{477, 260, 84, 21};
    const kit::Rect btnJointWar{387, 283, 174, 21};
    const kit::Rect btnBrain{352, 395, 116, 25};
    const kit::Rect btnBack{476, 395, 92, 25};

    std::vector<int> rivalList(const w2::Game& g, int me) const {
        std::vector<int> out;
        for (int i = 0; i < 8; i++) {
            if (i != me && playing(g, i)) out.push_back(i);
        }
        return out;
    }

    void draw() override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        if (!playing(g, other) || other == me) {
            auto rv = rivalList(g, me);
            if (!rv.empty()) other = rv[0];
        }
        const w2::Side& oSide = g.map->sides[other];
        const w2::Side& mySide = g.map->sides[me];
        const auto& prof = w2::ai::personality::forSide(oSide);
        auto att = dip::computeAttitude(g, other, me);

        kit::popup(box);
        const Font& fBig = kit::font(2);
        const Font& fSm = kit::font(0);

        // Top Title & Clickable Rival Switcher Shields
        gfx::setColor(1, 1, 1);
        fBig.colours(7, 0).draw("DIPLOMATIC PARLEY", 74, 50);

        auto rivals = rivalList(g, me);
        int shStartX = 568 - (int)rivals.size() * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            int rSide = rivals[idx];
            int sx = shStartX + (int)idx * 24;
            if (rSide == other) {
                kit::setPal(7);
                gfx::rectangle(gfx::FILL, sx - 2, 49, 20, 20);
                kit::setPal(0);
                kit::outline(sx - 3, 48, 22, 22);
            }
            smallShield(rSide, sx, 51);
        }

        // Ruler Bio & Stance Banner (72, 73, 496 x 64)
        drawSunkenPanel(72, 73, 496, 64);
        kit::shield(other, 80, 85);

        std::string archTag = "[" + cleanText(prof.archetype) + "]";
        int archW = fSm.width(archTag);
        int maxTitleW = 432 - archW - 10;
        std::string sideLabel = cleanText(oSide.name, false);
        int sideW = fBig.width(sideLabel);
        gfx::setColor(1, 1, 1);
        sideBannerFont(fBig, oSide).draw(sideLabel, 128, 77);
        if (sideW + 16 < maxTitleW) {
            std::string rulerPart = fitText(fBig, " - " + prof.name, maxTitleW - sideW, false);
            fBig.draw(rulerPart, 128 + sideW, 77);
        }
        kit::right(fSm.colours(7, 0), archTag, 560, 79);

        gfx::setColor(1, 1, 1);
        fSm.colours(5, 0).draw(fitText(fSm, "\"" + prof.motto + "\"", 432), 128, 97);

        int st = dip::state(g, me, other);
        bool allied = dip::isAllied(g, me, other);
        std::string stName =
                allied ? "ALLIED"
                       : (st == dip::PEACE ? "PEACE"
                                           : (st == dip::INTERMEDIATE ? "UNEASY" : "AT WAR"));
        int stPal = (allied || st == dip::PEACE) ? 10 : (st == dip::INTERMEDIATE ? 7 : 8);
        int tr = dip::truceTurns(g, me, other);
        int attPal = att.totalScore >= 15 ? 10 : (att.totalScore <= -15 ? 8 : 7);

        int cx = 128;
        cx += fSm.colours(stPal, 0).draw("[" + stName + "]", cx, 115);
        std::string midMeta = w2::fmt("  Truce:%d  Trust:%+d  Attitude:", tr, att.trustScore);
        cx += fSm.colours(1, 0).draw(midMeta, cx, 115);
        std::string attTail =
                fitText(fSm, w2::fmt(" %+d (%s)", att.totalScore, att.stanceSummary.c_str()),
                        std::max(20, 560 - cx));
        fSm.colours(attPal, 0).draw(attTail, cx, 115);

        // Left Panel: Attitude Ledger (72, 143, 300 x 167)
        int netPal = att.totalScore > 0 ? 10 : (att.totalScore < 0 ? 8 : 7);
        drawSunkenPanel(72, 143, 300, 167, "ATTITUDE LEDGER", w2::fmt("Total: %+d", att.totalScore),
                        netPal);

        gfx::setScissor(74, 163, 296, 145);
        if (att.factors.empty()) {
            fSm.colours(1, 0).draw("No active grievances or modifiers.", 82, 172);
        } else {
            int fy = 166;
            for (const auto& fac : att.factors) {
                if (fy > 294) break;
                std::string deltaStr = w2::fmt("%+d", fac.delta);
                int dCol = fac.delta > 0 ? 10 : (fac.delta < 0 ? 8 : 1);
                kit::right(fSm.colours(dCol, 0), deltaStr, 110, fy);

                auto fLines = wrapText(fSm, fac.label, 246);
                int shownLines = 0;
                for (const auto& ln : fLines) {
                    if (shownLines >= 2 || fy > 296) break;
                    fSm.draw(ln, 118, fy);
                    fy += 13;
                    shownLines++;
                }
                fy += 3;
            }
        }
        gfx::setScissor();

        // Right Panel: Treaty & Tribute Controls (380, 143, 188 x 167)
        drawSunkenPanel(380, 143, 188, 167, "PROPOSE TREATY");

        int myProp = dip::proposal(g, me, other);
        bool myAllyProp = dip::proposalAllied(g, me, other);
        int gOff = dip::goldOffer(g, me, other);
        int wTgt = dip::warTarget(g, me, other);
        std::string wTgtName =
                (wTgt >= 0 && wTgt < 8 && playing(g, wTgt)) ? g.map->sides[wTgt].name : "None";

        drawButton(btnAlly.x, btnAlly.y, btnAlly.w, btnAlly.h,
                   myAllyProp ? "* Propose Alliance *" : "Propose Alliance", myAllyProp, false, 11);
        drawButton(btnPeace.x, btnPeace.y, btnPeace.w, btnPeace.h,
                   (!myAllyProp && myProp == dip::PEACE) ? "* Propose Peace *" : "Propose Peace",
                   !myAllyProp && myProp == dip::PEACE, false, 11);
        drawButton(btnUneasy.x, btnUneasy.y, btnUneasy.w, btnUneasy.h,
                   myProp == dip::INTERMEDIATE ? "* Propose Uneasy *" : "Propose Uneasy",
                   myProp == dip::INTERMEDIATE, false, 13);
        drawButton(btnWar.x, btnWar.y, btnWar.w, btnWar.h,
                   myProp == dip::WAR ? "* Propose War *" : "Declare / Propose War",
                   myProp == dip::WAR, false, 9);

        drawButton(btnGoldMinus.x, btnGoldMinus.y, btnGoldMinus.w, btnGoldMinus.h, "Gift -50g",
                   false, gOff <= 0, 13);
        drawButton(btnGoldPlus.x, btnGoldPlus.y, btnGoldPlus.w, btnGoldPlus.h,
                   w2::fmt("Gift +%dg", gOff + 50), gOff > 0, gOff + 50 > mySide.gold, 13);

        drawButton(btnJointWar.x, btnJointWar.y, btnJointWar.w, btnJointWar.h,
                   "War Target: " + wTgtName, wTgt != w2::NONE, false, 9);

        // Bottom Panel: Diplomatic Forecast (72, 316, 496 x 72)
        drawSunkenPanel(72, 316, 496, 72, "DIPLOMATIC FORECAST",
                        w2::fmt("Treasury: %d gp   Gift Offer: %d gp", mySide.gold, gOff), 5);

        gfx::setScissor(74, 336, 492, 50);
        auto wrapped = wrapText(fSm, att.whatItTakes, 478);
        int wy = 339;
        for (size_t i = 0; i < wrapped.size() && i < 2; i++) {
            fSm.draw(wrapped[i], 80, wy);
            wy += 14;
        }
        if (allied && dip::alliedTargetCity(g, other) != w2::NONE) {
            if (const w2::City* ac = g.map->city(dip::alliedTargetCity(g, other))) {
                std::string bc = fitText(
                        fSm,
                        w2::fmt("Allied War Target: %s (%d, %d)", ac->name.c_str(), ac->x, ac->y),
                        478);
                fSm.colours(10, 0).draw(bc, 80, 369);
            }
        } else if (wy <= 368) {
            int theirProp = dip::proposal(g, other, me);
            bool theirAlly = dip::proposalAllied(g, other, me);
            std::string theirStr =
                    theirAlly ? "Alliance"
                              : (theirProp == dip::PEACE
                                         ? "Peace"
                                         : (theirProp == dip::INTERMEDIATE ? "Uneasy" : "War"));
            fSm.colours(1, 0).draw(
                    fitText(fSm, "Their current posture toward you: " + theirStr, 478), 80, 369);
        }
        gfx::setScissor();

        // Footer Hints & Action Buttons
        fSm.colours(1, 0).draw("Left/Right or click top shields to switch", 74, 401);
        drawButton(btnBrain.x, btnBrain.y, btnBrain.w, btnBrain.h, "AI Brain (F10)");
        drawButton(btnBack.x, btnBack.y, btnBack.w, btnBack.h, "Back");
    }

    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        auto rivals = rivalList(g, me);
        int shStartX = 568 - (int)rivals.size() * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            kit::Rect sh{shStartX + (int)idx * 24 - 2, 49, 20, 20};
            if (sh.contains(x, y)) {
                other = rivals[idx];
                return;
            }
        }
        if (btnAlly.contains(x, y)) {
            dip::propose(g, me, other, dip::PEACE);
            dip::proposeAlliance(g, me, other, true);
        } else if (btnPeace.contains(x, y)) {
            dip::propose(g, me, other, dip::PEACE);
            dip::proposeAlliance(g, me, other, false);
        } else if (btnUneasy.contains(x, y)) {
            dip::propose(g, me, other, dip::INTERMEDIATE);
        } else if (btnWar.contains(x, y)) {
            dip::propose(g, me, other, dip::WAR);
            dip::setGoldOffer(g, me, other, 0);
        } else if (btnGoldMinus.contains(x, y)) {
            int cur = dip::goldOffer(g, me, other);
            dip::setGoldOffer(g, me, other, std::max(0, cur - 50));
        } else if (btnGoldPlus.contains(x, y)) {
            int cur = dip::goldOffer(g, me, other);
            if (cur + 50 <= g.map->sides[me].gold) {
                dip::setGoldOffer(g, me, other, cur + 50);
            }
        } else if (btnJointWar.contains(x, y)) {
            int cur = dip::warTarget(g, me, other);
            int next = w2::NONE;
            int start = (cur == w2::NONE) ? 0 : cur + 1;
            for (int s = start; s < 8; s++) {
                if (s != me && s != other && playing(g, s)) {
                    next = s;
                    break;
                }
            }
            dip::setWarTarget(g, me, other, next);
        } else if (btnBrain.contains(x, y)) {
            openBrainInspector(other);
        } else if (btnBack.contains(x, y)) {
            bool act = returnToAction;
            kit::pop(this);
            if (act)
                openAction();
            else
                openReport();
        }
    }

    void keypressed(const std::string& key) override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        if (key == "left" || key == "right") {
            auto rivals = rivalList(g, me);
            if (!rivals.empty()) {
                auto it = std::find(rivals.begin(), rivals.end(), other);
                int idx = (it == rivals.end()) ? 0 : (int)(it - rivals.begin());
                int n = (int)rivals.size();
                other = rivals[(idx + (key == "right" ? 1 : n - 1)) % n];
            }
            return;
        }
        if (key == "f10") {
            openBrainInspector(other);
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") {
            bool act = returnToAction;
            kit::pop(this);
            if (act)
                openAction();
            else
                openReport();
        }
    }
};

// ------------------------------------------------------------------ Turn-Start Envoy Missive Modal
struct MissiveModal : kit::Modal {
    w2::DiplomaticMissive m;
    kit::Done after;

    const kit::Rect box{88, 72, 464, 316};
    const kit::Rect btnAccept{100, 348, 148, 28};
    const kit::Rect btnParley{258, 348, 136, 28};
    const kit::Rect btnDecline{404, 348, 136, 28};

    void finish() {
        auto cb = after;
        kit::pop(this);
        pollMissives(cb);
    }

    void draw() override {
        w2::Game& g = *G.g;
        const w2::Side& from = g.map->sides[std::clamp(m.fromSide, 0, 7)];
        kit::popup(box);

        const Font& fBig = kit::font(2);
        const Font& fSm = kit::font(0);

        // Top Envoy Banner
        drawSunkenPanel(100, 84, 440, 56);
        kit::shield(m.fromSide, 108, 92);

        gfx::setColor(1, 1, 1);
        sideBannerFont(fBig, from).draw(fitText(fBig, m.headline, 372, false), 156, 90);

        std::string dealTag = m.proposeAlliance
                                      ? "Proposal: Military Alliance (Shared Vision)"
                                      : (m.proposedState == dip::PEACE
                                                 ? "Proposal: Peace Treaty"
                                                 : (m.proposedState == dip::INTERMEDIATE
                                                            ? "Notice: Border Warning / Uneasy"
                                                            : "Declaration"));
        if (m.goldDemand > 0)
            dealTag += w2::fmt("  -  Tribute Demand: %d gp", m.goldDemand);
        else if (m.goldDemand < 0)
            dealTag += w2::fmt("  -  Tribute Gift: +%d gp", -m.goldDemand);
        int dealPal = (m.proposeAlliance || m.proposedState == dip::PEACE)
                              ? 10
                              : (m.proposedState == dip::INTERMEDIATE ? 7 : 8);
        fSm.colours(dealPal, 0).draw(fitText(fSm, dealTag, 372), 156, 114);

        // Envoy Message Box
        drawSunkenPanel(100, 146, 440, 108, "ROYAL ENVOY DISPATCH");
        gfx::setScissor(102, 166, 436, 86);
        auto bodyLines = wrapText(fBig, m.body, 420, false);
        int by = 170;
        for (const auto& ln : bodyLines) {
            if (by > 236) break;
            fBig.draw(ln, 108, by);
            by += 19;
        }
        gfx::setScissor();

        // Diplomatic Context Box
        drawSunkenPanel(100, 260, 440, 78, "DIPLOMATIC CONTEXT",
                        w2::fmt("Your Treasury: %d gp", G.player ? G.player->gold : 0), 5);
        gfx::setScissor(102, 280, 436, 56);
        auto ctxLines = wrapText(fSm, m.reasonSummary, 420);
        int cy = 283;
        for (const auto& ln : ctxLines) {
            if (cy > 322) break;
            fSm.draw(ln, 108, cy);
            cy += 14;
        }
        gfx::setScissor();

        int myGold = G.player ? G.player->gold : 0;
        bool canAfford = (m.goldDemand <= 0 || myGold >= m.goldDemand);
        drawButton(btnAccept.x, btnAccept.y, btnAccept.w, btnAccept.h,
                   m.goldDemand > 0 ? w2::fmt("Pay %dg + Accept", m.goldDemand) : "Accept Proposal",
                   canAfford, !canAfford, 11);
        drawButton(btnParley.x, btnParley.y, btnParley.w, btnParley.h, "Open Parley");
        drawButton(btnDecline.x, btnDecline.y, btnDecline.w, btnDecline.h, "Decline", false, false,
                   9);
    }

    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = activeViewerIndex(g);
        if (btnAccept.contains(x, y)) {
            if (m.goldDemand > 0 && g.map->sides[me].gold < m.goldDemand) return;
            if (m.goldDemand > 0) {
                g.map->sides[me].gold -= m.goldDemand;
                g.map->sides[m.fromSide].gold += m.goldDemand;
            }
            dip::propose(g, me, m.fromSide, m.proposedState);
            dip::propose(g, m.fromSide, me, m.proposedState);
            if (m.proposeAlliance) {
                dip::setAllied(g, me, m.fromSide, true);
                dip::setTruce(g, me, m.fromSide, 15);
                dip::addTrust(g, m.fromSide, me, 18);
            } else if (m.proposedState == dip::PEACE) {
                g.diplomacy.state[me * 8 + m.fromSide] = dip::PEACE;
                g.diplomacy.state[m.fromSide * 8 + me] = dip::PEACE;
                dip::setTruce(g, me, m.fromSide, 10);
                dip::addTrust(g, m.fromSide, me, 10);
            }
            if (m.jointWarTarget != w2::NONE && playing(g, m.jointWarTarget)) {
                g.diplomacy.state[me * 8 + m.jointWarTarget] = dip::WAR;
                g.diplomacy.state[m.jointWarTarget * 8 + me] = dip::WAR;
                dip::propose(g, me, m.jointWarTarget, dip::WAR);
            }
            front::stratDirty();
            finish();
        } else if (btnParley.contains(x, y)) {
            int from = m.fromSide;
            kit::pop(this);
            openParley(from, false);
        } else if (btnDecline.contains(x, y)) {
            finish();
        }
    }

    void keypressed(const std::string& key) override {
        if (key == "escape")
            finish();
        else if (key == "return" || key == "kpenter") {
            mousepressed(btnAccept.x + 5, btnAccept.y + 5, 1);
        }
    }
};

// ------------------------------------------------------------------ F10 AI Brain & Policy
// Inspector
struct BrainInspectorModal : kit::Modal {
    int sideIdx = 0;
    const kit::Rect box{44, 34, 552, 412};
    const kit::Rect btnToggleEngine{380, 71, 128, 19};
    const kit::Rect btnClose{514, 71, 64, 19};

    void draw() override {
        w2::Game& g = *G.g;
        if (!playing(g, sideIdx)) {
            for (int s = 0; s < 8; s++) {
                if (playing(g, s) && g.map->sides[s].computer) {
                    sideIdx = s;
                    break;
                }
            }
            if (!playing(g, sideIdx)) sideIdx = activeViewerIndex(g);
        }
        w2::Side& s = g.map->sides[sideIdx];
        const auto& prof = w2::ai::personality::forSide(s);
        const auto& b = w2::ai::belief::ensure(s);

        kit::popup(box);
        const Font& fBig = kit::font(2);
        const Font& fSm = kit::font(0);

        gfx::setColor(1, 1, 1);
        fBig.colours(7, 0).draw("AI BRAIN INSPECTOR (F10)", 56, 42);

        // Top row of clickable faction shields
        for (int i = 0; i < 8; i++) {
            if (!playing(g, i)) continue;
            int sx = 376 + i * 26;
            if (i == sideIdx) {
                kit::setPal(7);
                gfx::rectangle(gfx::FILL, sx - 2, 41, 20, 20);
                kit::setPal(0);
                kit::outline(sx - 3, 40, 22, 22);
            }
            smallShield(i, sx, 43);
        }

        // Faction Bio & Value Head Summary Banner (56, 67, 528 x 58)
        drawSunkenPanel(56, 67, 528, 58);
        kit::shield(sideIdx, 64, 76);

        std::string sideTitle = cleanText(s.name, false);
        int stW = fBig.width(sideTitle);
        gfx::setColor(1, 1, 1);
        sideBannerFont(fBig, s).draw(fitText(fBig, sideTitle, 260, false), 112, 71);
        if (stW + 16 < 262) {
            fBig.draw(fitText(fBig, " - " + prof.name, 262 - stW, false), 112 + stW, 71);
        }
        fSm.colours(5, 0).draw(fitText(fSm, "\"" + prof.motto + "\"", 464), 112, 91);

        int exploredPct = (int)std::round((1.0f - b.frontierRatio) * 100.0f);
        std::string evalStr =
                w2::fmt("[%s]  V(s):%+.2f  Expl:%d%%  Cit:%d  Gold:%d gp", prof.archetype.c_str(),
                        s.lastBoardValue, exploredPct, (int)b.visibleCities.size(), s.gold);
        int valPal = s.lastBoardValue >= 0.05f ? 10 : (s.lastBoardValue <= -0.05f ? 8 : 7);
        fSm.colours(valPal, 0).draw(fitText(fSm, evalStr, 464), 112, 106);

        drawButton(btnToggleEngine.x, btnToggleEngine.y, btnToggleEngine.w, btnToggleEngine.h,
                   s.classicAi ? "AI: 1993 Classic" : "AI: Learnable RL", !s.classicAi, false, 11);
        drawButton(btnClose.x, btnClose.y, btnClose.w, btnClose.h, "Close");

        // Left Panel: 10D Personality Vector Trait Bars (56, 130, 212 x 165)
        drawSunkenPanel(56, 130, 212, 165, "10D PERSONALITY TRAITS");
        static const char* const TRAIT_LABELS[10] = {
            "Aggression", "Honour",  "Avarice", "Scorched", "HeroFocus",
            "Coalition",  "Cavalry", "Fliers",  "Siege",    "Caution"};
        for (int t = 0; t < 10; t++) {
            int ty = 150 + t * 14;
            fSm.draw(TRAIT_LABELS[t], 64, ty);
            int barX = 160, barW = 98, midX = barX + barW / 2;
            kit::setPal(3);
            gfx::rectangle(gfx::FILL, barX, ty + 3, barW, 8);
            kit::bevel(barX, ty + 3, barW, 8, 0, 2);
            float val = std::clamp(prof.traits[t], -1.0f, 1.0f);
            int fillW = (int)std::round(std::abs(val) * (barW / 2 - 2));
            kit::setPal(val >= 0 ? 10 : 8);
            if (fillW > 0) {
                if (val >= 0)
                    gfx::rectangle(gfx::FILL, midX + 1, ty + 4, fillW, 6);
                else
                    gfx::rectangle(gfx::FILL, midX - fillW, ty + 4, fillW, 6);
            }
            kit::setPal(15);
            gfx::rectangle(gfx::FILL, midX, ty + 2, 1, 10);
        }

        // Right Panel: Fair-Information Belief Summary (276, 130, 308 x 165)
        drawSunkenPanel(
                276, 130, 308, 165, "BELIEF MEMORY",
                w2::fmt("Fog:%d Stacks:%d", (int)b.fogFrontiers.size(), (int)b.stacksByTile.size()),
                5);
        gfx::setScissor(278, 150, 304, 143);
        fSm.colours(1, 0).draw("Rival", 284, 151);
        kit::right(fSm.colours(1, 0), "Cit", 434, 151);
        kit::right(fSm.colours(1, 0), "Str", 478, 151);
        kit::right(fSm.colours(1, 0), "Us / Them", 574, 151);
        kit::setPal(0);
        gfx::rectangle(gfx::FILL, 282, 165, 296, 1);

        int rby = 169;
        for (int o = 0; o < 8; o++) {
            if (o == sideIdx || !playing(g, o)) continue;
            if (rby > 279) break;
            smallShield(o, 284, rby - 1);
            std::string rName = fitText(fSm, g.map->sides[o].name, 106);
            fSm.draw(rName, 304, rby);
            kit::right(fSm.colours(7, 0), w2::fmt("%d", b.visibleCitiesBySide[o]), 434, rby);
            kit::right(fSm.colours(5, 0), w2::fmt("%.1f", b.seenEnemyAvgStrength[o]), 478, rby);
            std::string bdr =
                    w2::fmt("%.0f / %.0f", b.myBorderStrength[o], b.theirBorderStrength[o]);
            int bPal = b.myBorderStrength[o] >= b.theirBorderStrength[o] ? 10 : 8;
            kit::right(fSm.colours(bPal, 0), bdr, 574, rby);
            rby += 17;
        }
        gfx::setScissor();

        // Bottom Panel: Top Scored Neural Policy Decisions (56, 301, 528 x 135)
        drawSunkenPanel(56, 301, 528, 135, "RECENT NEURAL POLICY DECISIONS",
                        "Left/Right: Switch Side", 1);
        gfx::setScissor(58, 321, 524, 113);
        if (s.aiThoughts.empty()) {
            fSm.colours(1, 0).draw(
                    "No neural thoughts recorded yet (advance a turn to watch this AI act).", 66,
                    330);
        } else {
            int ty = 323;
            int shown = 0;
            for (int pass = 0; pass < 2; pass++) {
                for (const auto& th : s.aiThoughts) {
                    bool isDiplo = (th.category == "Diplomacy");
                    if ((pass == 0 && isDiplo) || (pass == 1 && !isDiplo)) continue;
                    if (shown >= 7 || ty > 420) break;
                    int catPal = 15;
                    if (th.stageNear)
                        catPal = 7;
                    else if (th.category == "Economy")
                        catPal = 5;
                    else if (th.category == "Spoils")
                        catPal = 8;
                    else if (th.category == "Diplomacy")
                        catPal = 10;

                    std::string shortCat =
                            th.category == "Mission"
                                    ? "Move"
                                    : (th.category == "Economy"
                                               ? "Econ"
                                               : (th.category == "Diplomacy" ? "Diplo"
                                                                             : th.category));
                    std::string actorCompact = th.actorName;
                    size_t uPos = actorCompact.find("-unit stack");
                    if (uPos != std::string::npos) actorCompact.replace(uPos, 11, "u");
                    std::string tag = "[" + shortCat + "] ";
                    int tagW = fSm.colours(catPal, 0).draw(tag, 64, ty);
                    std::string rest =
                            fitText(fSm, actorCompact + " -> " + th.targetName + ": " + th.detail,
                                    512 - tagW);
                    fSm.draw(rest, 64 + tagW, ty);
                    ty += 15;
                    shown++;
                }
            }
        }
        gfx::setScissor();
    }

    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        for (int i = 0; i < 8; i++) {
            if (!playing(g, i)) continue;
            kit::Rect sh{374 + i * 26, 41, 20, 20};
            if (sh.contains(x, y)) {
                sideIdx = i;
                return;
            }
        }
        if (btnToggleEngine.contains(x, y)) {
            g.map->sides[sideIdx].classicAi = !g.map->sides[sideIdx].classicAi;
            return;
        }
        if (btnClose.contains(x, y)) {
            kit::pop(this);
        }
    }

    void keypressed(const std::string& key) override {
        w2::Game& g = *G.g;
        if (key == "left" || key == "right") {
            int dir = (key == "right") ? 1 : 7;
            for (int step = 1; step <= 8; step++) {
                int cand = (sideIdx + dir * step) % 8;
                if (playing(g, cand)) {
                    sideIdx = cand;
                    break;
                }
            }
            return;
        }
        if (key == "f10" || key == "escape" || key == "return" || key == "kpenter") {
            kit::pop(this);
        }
    }
};

}  // namespace

void open() {
    if (G.g->map->options.diplomacy == 0) return;
    if (G.modernUi)
        openReport();
    else
        openClassicReport();
}

void action() {
    if (G.g->map->options.diplomacy == 0) return;
    if (G.modernUi)
        openAction();
    else
        openClassicAction();
}

void openParley(int otherSide, bool returnToAction) {
    if (!G.modernUi || !G.g || otherSide < 0 || otherSide >= 8) return;
    auto d = std::make_shared<ParleyModal>();
    d->other = otherSide;
    d->returnToAction = returnToAction;
    kit::push(d);
}

void pollMissives(kit::Done after) {
    if (!G.modernUi) {
        if (G.player) G.player->missives.clear();
        if (after) after();
        return;
    }
    if (!G.g || !G.player || G.player->computer || G.player->missives.empty()) {
        if (after) after();
        return;
    }
    w2::DiplomaticMissive m = G.player->missives.front();
    G.player->missives.erase(G.player->missives.begin());
    if (!playing(*G.g, m.fromSide)) {
        pollMissives(after);
        return;
    }
    auto d = std::make_shared<MissiveModal>();
    d->m = std::move(m);
    d->after = after;
    kit::push(d);
}

void openBrainInspector(int initialSide) {
    if (!G.modernUi || !G.g) return;
    auto d = std::make_shared<BrainInspectorModal>();
    if (initialSide >= 0 && initialSide < 8 && playing(*G.g, initialSide)) {
        d->sideIdx = initialSide;
    } else {
        for (int i = 0; i < 8; i++) {
            if (playing(*G.g, i) && G.g->map->sides[i].computer) {
                d->sideIdx = i;
                break;
            }
        }
    }
    kit::push(d);
}

int buttonFor(w2::Game& g, int me) {
    if (g.map->options.diplomacy == 0) return w2::NONE;
    bool friendlier = false, hostile = false;
    for (int s = 0; s < 8; s++) {
        if (s != me) {
            int st = dip::state(g, s, me), p = dip::proposal(g, s, me);
            if (p != st) {
                if (p < st)
                    friendlier = true;
                else
                    hostile = true;
            } else if (G.modernUi && dip::proposalAllied(g, s, me) && !dip::isAllied(g, s, me)) {
                friendlier = true;
            }
        }
    }
    if (!(friendlier || hostile)) return 183;
    return hostile ? 184 : 185;
}

}  // namespace diplomacyui
