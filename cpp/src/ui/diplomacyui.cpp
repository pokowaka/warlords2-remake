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
const kit::Rect kR{80, 60, 480, 350};  // popup 11
const int kReport = 21, kDone = 368, kAction = 369;
const int kAct = 22, kOk = 370, kBack = 371;
const int kOffer[3] = {372, 380, 388};  // a row of 7 each

bool Playing(const w2::Game& g, int i) {
    return i >= 0 && i < 8 && g.map->sides[i].inUse && g.map->sides[i].alive;
}

int ActiveViewerIndex(const w2::Game& g) {
    if (G.player && G.player->index >= 0 && G.player->index < 8) return G.player->index;
    for (int i = 0; i < 8; i++) {
        if (Playing(g, i)) return i;
    }
    return 0;
}

void Diplom(int sx, int sy, int w, int h, int x, int y) {
    if (auto art = G.screen->artFor(47)) {
        gfx::setColor(1, 1, 1);
        gfx::draw(art->image, gfx::newQuad(sx, sy, w, h), x, y);
    }
}

void SmallShield(int side, int x, int y) {
    gfx::setColor(1, 1, 1);
    gfx::draw(G.shieldsImg, gfx::newQuad(side * 40 + 24, 46, 16, 16), x, y);
}

void Blank(int x, int y) {
    kit::setPal(0);
    kit::outline(x, y, 40, 40);
    kit::setPal(2);
    gfx::rectangle(gfx::FILL, x + 1, y + 1, 38, 38);
}

// ---------------------------------------------------------------------------
// 1993 Classic Diplomacy UI (default unless --modern-ui is enabled)
// ---------------------------------------------------------------------------

void OpenClassicAction();

struct ClassicReport : kit::Modal {
    screen::View v;
    std::map<int, std::string> titles;
    std::vector<w2::Side*> order;
    void draw() override {
        w2::Game& g = *G.g;
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6b, 0), 320, 64);
        kit::setPal(1);
        gfx::rectangle(gfx::FILL, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 120 + 32 * i, 110, 1, 261);
        gfx::rectangle(gfx::FILL, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(g, i)) {
                SmallShield(i, 128 + 32 * i, 117);
                SmallShield(i, 96, 146 + 29 * i);
            }
        }
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    Diplom(384, 29, 32, 29, x, y);
                else if (Playing(g, col) && Playing(g, row)) {
                    int st = dip::state(g, col, row);
                    if (st == dip::WAR)
                        Diplom(384, 0, 32, 29, x, y);
                    else if (st == dip::PEACE)
                        Diplom(320, 0, 32, 29, x, y);
                }
            }
        }
        kit::setPal(0);
        kit::outline(392, 110, 160, 262);
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        kit::centred(f, kit::text(0x6d, 0), 472, 116);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(order[i]->index, 400, y);
            gfx::setColor(1, 1, 1);
            f.draw(titles[order[i]->index], 432, y);
        }
        kit::drawControls(v, hidden);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y, hidden);
        if (!c) return;
        if (c->id == kDone)
            kit::pop(this);
        else if (c->id == kAction) {
            kit::pop(this);
            OpenClassicAction();
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

void OpenClassicReport() {
    auto d = std::make_shared<ClassicReport>();
    d->v = kit::view(kReport);
    d->view = &d->v;
    d->v.state[kDone] = d->v.state[kAction] = w2::uidata::NORMAL;
    if (!w2::game::hasActiveHuman(*G.g)) d->hidden.insert(kAction);
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
        int me = ActiveViewerIndex(g);
        const w2::Side& my_side = g.map->sides[me];
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6c, 0), 320, 64);
        kit::shield(me, 96, 111);
        const Font& f = kit::font(2);
        gfx::setColor(1, 1, 1);
        f.draw(my_side.name, 144, 122);
        const int ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) f.draw(kit::text(0x6e, i), 104, ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::shield(other, x, 111);
            if (!Playing(g, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(x, y);
            } else {
                int st = dip::state(g, other, me);
                Diplom(80 + 120 * st, 45, 40, 40, x, 151);
                int theirs = dip::proposal(g, other, me);
                if (theirs == st)
                    Blank(x, 191);
                else
                    Diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                int mine = dip::proposal(g, me, other);
                for (int k = 0; k <= 2; k++)
                    Diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
            }
        }
        std::set<int> h;
        for (int k = 0; k <= 2; k++)
            for (int i = 0; i <= 6; i++) h.insert(kOffer[k] + i);
        kit::drawControls(v, h);
    }
    void mousepressed(int x, int y, int) override {
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kOk) {
            kit::pop(this);
            return;
        }
        if (c->id == kBack) {
            kit::pop(this);
            OpenClassicReport();
            return;
        }
        int me = ActiveViewerIndex(*G.g);
        for (int k = 0; k <= 2; k++) {
            int i = c->id - kOffer[k];
            if (i >= 0 && i < 7 && i < static_cast<int>(others.size()))
                dip::propose(*G.g, me, others[i], k);
        }
    }
    void keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::pop(this);
    }
};

void OpenClassicAction() {
    auto d = std::make_shared<ClassicAction>();
    d->v = kit::view(kAct);
    d->view = &d->v;
    for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::NORMAL;
    int me = ActiveViewerIndex(*G.g);
    for (int i = 0; i < 8; i++)
        if (i != me) d->others.push_back(i);
    kit::push(d);
}

// Sanitize UTF-8 punctuation and bitmap-font repurposed glyphs:
// In SSG's TEXT.FNT (smallFont == true), ASCII '&' (38) is the menu checkmark
// glyph and '`' (96) is a solid block.
std::string CleanText(const std::string& raw, bool small_font = true) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        // UTF-8 em-dash / en-dash (E2 80 93 / E2 80 94) and curly quotes
        if (c == 0xe2 && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0x80) {
            unsigned char c2 = static_cast<unsigned char>(raw[i + 2]);
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
        if (small_font && c == '&') {
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
        out += static_cast<char>(c);
    }
    return out;
}

std::string FitText(const Font& f, const std::string& raw, int max_w, bool small_font = true) {
    std::string s = CleanText(raw, small_font);
    if (max_w <= 0) return "";
    if (f.width(s) <= max_w) return s;
    const std::string ell = "..";
    int ell_w = f.width(ell);
    if (ell_w >= max_w) return "";
    while (!s.empty() && f.width(s) + ell_w > max_w) {
        s.pop_back();
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + ell;
}

std::vector<std::string> WrapText(const Font& f, const std::string& raw, int max_w,
                                  bool small_font = true) {
    std::string cleaned = CleanText(raw, small_font);
    std::vector<std::string> lines;
    if (max_w <= 0 || cleaned.empty()) return lines;
    std::istringstream iss(cleaned);
    std::string word, cur;
    while (iss >> word) {
        if (f.width(word) > max_w) {
            word = FitText(f, word, max_w, false);
        }
        std::string test = cur.empty() ? word : cur + " " + word;
        if (f.width(test) <= max_w || cur.empty()) {
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
void DrawSunkenPanel(int x, int y, int w, int h, const std::string& left_hdr = "",
                     const std::string& right_hdr = "", int right_pal = 5) {
    kit::setPal(4);  // deep charcoal interior (31,31,31)
    gfx::rectangle(gfx::FILL, x, y, w, h);
    kit::bevel(x, y, w, h, 0, 1);  // sunken 3D stone bevel: black top/left, silver bottom/right
    if (!left_hdr.empty() || !right_hdr.empty()) {
        kit::setPal(3);  // subtle slate header band (44,44,44)
        gfx::rectangle(gfx::FILL, x + 1, y + 1, w - 2, 17);
        kit::setPal(0);
        gfx::rectangle(gfx::FILL, x + 1, y + 18, w - 2, 1);
        const Font& f_sm = kit::font(0);
        gfx::setColor(1, 1, 1);
        int avail_w = w - 14;
        std::string r_clean = right_hdr.empty() ? "" : FitText(f_sm, right_hdr, avail_w / 2 + 36);
        int r_w = r_clean.empty() ? 0 : f_sm.width(r_clean) + 10;
        if (!left_hdr.empty()) {
            std::string l_clean = FitText(f_sm, left_hdr, std::max(24, avail_w - r_w));
            f_sm.colours(7, 0).draw(l_clean, x + 7, y + 3);
        }
        if (!r_clean.empty()) {
            kit::right(f_sm.colours(right_pal, 0), r_clean, x + w - 7, y + 3);
        }
    }
}

// Raised 3D stone button with crisp black-outlined text and semantic active fill.
void DrawButton(int x, int y, int w, int h, const std::string& label, bool active = false,
                bool disabled = false, int accent_pal = 11) {
    kit::setPal(0);
    kit::outline(x, y, w, h);
    kit::setPal(disabled ? 3 : (active ? accent_pal : 2));
    gfx::rectangle(gfx::FILL, x + 1, y + 1, w - 2, h - 2);
    kit::bevel(x + 1, y + 1, w - 2, h - 2, active ? 7 : (disabled ? 2 : 1), 0);

    const Font& f_sm = kit::font(0);
    int glyph = disabled ? 2 : (active ? 7 : 15);
    const Font& f = f_sm.colours(glyph, 0);
    std::string fitted = FitText(f_sm, label, w - 8);
    gfx::setColor(1, 1, 1);
    kit::centred(f, fitted, x + w / 2, y + (h - f.lineHeight) / 2);
}

// High-contrast heraldic font for dark slate panels (lifts Lich King black/navy to crisp readable
// tones).
const Font& SideBannerFont(const Font& f_big, const w2::Side& s) {
    if (s.colour == 0) return f_big.colours(15, s.edge == 0 ? 9 : s.edge);
    if (s.colour == 6) return f_big.colours(5, 0);
    return f_big.colours(s.colour, s.edge);
}

void OpenReport();
void OpenAction();

struct Report : kit::Modal {
    screen::View v;
    std::map<int, std::string> titles;
    std::vector<w2::Side*> order;
    void draw() override {
        w2::Game& g = *G.g;
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6b, 0), 320, 64);
        kit::setPal(1);
        gfx::rectangle(gfx::FILL, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 120 + 32 * i, 110, 1, 261);
        gfx::rectangle(gfx::FILL, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::rectangle(gfx::FILL, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(g, i)) {
                SmallShield(i, 128 + 32 * i, 117);
                SmallShield(i, 96, 146 + 29 * i);
            }
        }
        const Font& tf = kit::font(0);
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    Diplom(384, 29, 32, 29, x, y);
                else if (Playing(g, col) && Playing(g, row)) {
                    int st = dip::state(g, col, row);
                    if (st == dip::WAR)
                        Diplom(384, 0, 32, 29, x, y);
                    else if (st == dip::PEACE) {
                        Diplom(320, 0, 32, 29, x, y);
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
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(order[i]->index, 400, y);
            gfx::setColor(1, 1, 1);
            std::string r_title = FitText(f, titles[order[i]->index], 120, false);
            f.draw(r_title, 424, y);
        }
        // Centered cleanly between ACTION (96..160) and DONE (480..544)
        gfx::setColor(1, 1, 1);
        kit::centred(tf.colours(7, 0), "Click any shield to Parley  -  F10: AI Brain", 320, 384);
        kit::drawControls(v);
    }
    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        for (int i = 0; i < 8; i++) {
            if (i == me || !Playing(g, i)) continue;
            kit::Rect top_sh{120 + 32 * i, 110, 32, 28};
            kit::Rect left_sh{88, 139 + 29 * i, 32, 29};
            if (top_sh.contains(x, y) || left_sh.contains(x, y)) {
                kit::pop(this);
                openParley(i, false);
                return;
            }
        }
        for (size_t i = 0; i < order.size(); i++) {
            int s_idx = order[i]->index;
            if (s_idx == me) continue;
            kit::Rect rate_row{394, 142 + 29 * static_cast<int>(i), 156, 26};
            if (rate_row.contains(x, y)) {
                kit::pop(this);
                openParley(s_idx, false);
                return;
            }
        }
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kDone)
            kit::pop(this);
        else if (c->id == kAction) {
            kit::pop(this);
            OpenAction();
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

void OpenReport() {
    auto d = std::make_shared<Report>();
    d->v = kit::view(kReport);
    d->view = &d->v;
    d->v.state[kDone] = d->v.state[kAction] = w2::uidata::NORMAL;
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
        int me = ActiveViewerIndex(g);
        const w2::Side& my_side = g.map->sides[me];
        kit::popup(kR);
        gfx::setColor(1, 1, 1);
        kit::centred(kit::font(1), kit::text(0x6c, 0), 320, 64);
        kit::shield(me, 96, 111);
        const Font& f = kit::font(2);
        const Font& tf = kit::font(0);
        gfx::setColor(1, 1, 1);
        SideBannerFont(f, my_side).draw(FitText(f, my_side.name, 122, false), 144, 122);
        const int ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) f.draw(kit::text(0x6e, i), 104, ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::shield(other, x, 111);
            if (!Playing(g, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(x, y);
            } else {
                int st = dip::state(g, other, me);
                Diplom(80 + 120 * st, 45, 40, 40, x, 151);
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
                bool their_ally = dip::proposalAllied(g, other, me);
                if (theirs == st && (!their_ally || dip::isAllied(g, other, me)))
                    Blank(x, 191);
                else {
                    Diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                    if (their_ally) {
                        gfx::setColor(1, 1, 1);
                        tf.colours(7, 0).draw("ALLY", x + 6, 193);
                    }
                }

                int mine = dip::proposal(g, me, other);
                bool my_ally = dip::proposalAllied(g, me, other);
                for (int k = 0; k <= 2; k++) {
                    Diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
                }
                if (my_ally && mine == dip::PEACE) {
                    kit::setPal(10);
                    kit::outline(x + 1, 246, 38, 38);
                    gfx::setColor(1, 1, 1);
                    tf.colours(7, 0).draw("ALLY", x + 6, 247);
                }
                int g_off = dip::goldOffer(g, me, other);
                if (g_off > 0) {
                    gfx::setColor(1, 1, 1);
                    tf.colours(7, 0).draw(w2::fmt("+%dg", g_off), x + 3, 271);
                }
            }
        }
        // Centered cleanly between BACK (96..160) and OK (480..544)
        gfx::setColor(1, 1, 1);
        kit::centred(tf.colours(7, 0), "Click a top shield to Parley  -  F10: AI Brain", 320, 384);
        std::set<int> h;
        for (int k = 0; k <= 2; k++)
            for (int i = 0; i <= 6; i++) h.insert(kOffer[k] + i);
        kit::drawControls(v, h);
    }
    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            if (!Playing(g, other)) continue;
            kit::Rect sh_rect{272 + 40 * static_cast<int>(col), 108, 40, 43};
            if (sh_rect.contains(x, y)) {
                kit::pop(this);
                openParley(other, true);
                return;
            }
        }
        auto c = kit::controlAt(v, x, y);
        if (!c) return;
        if (c->id == kOk) {
            kit::pop(this);
            return;
        }
        if (c->id == kBack) {
            kit::pop(this);
            OpenReport();
            return;
        }
        for (int k = 0; k <= 2; k++) {
            int i = c->id - kOffer[k];
            if (i >= 0 && i < 7 && i < static_cast<int>(others.size())) {
                int other = others[i];
                if (k == dip::PEACE && dip::proposal(g, me, other) == dip::PEACE) {
                    bool next_ally = !dip::proposalAllied(g, me, other);
                    dip::proposeAlliance(g, me, other, next_ally);
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

void OpenAction() {
    auto d = std::make_shared<Action>();
    d->v = kit::view(kAct);
    d->view = &d->v;
    int me = ActiveViewerIndex(*G.g);
    for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::NORMAL;
    for (int i = 0; i < 8; i++)
        if (i != me) d->others.push_back(i);
    kit::push(d);
}

// ------------------------------------------------------------------ Parley & Attitude Ledger
struct ParleyModal : kit::Modal {
    int other = 0;
    bool return_to_action = false;

    const kit::Rect box{60, 44, 520, 384};
    const kit::Rect btn_ally{387, 166, 174, 21};
    const kit::Rect btn_peace{387, 189, 174, 21};
    const kit::Rect btn_uneasy{387, 212, 174, 21};
    const kit::Rect btn_war{387, 235, 174, 21};
    const kit::Rect btn_gold_minus{387, 260, 84, 21};
    const kit::Rect btn_gold_plus{477, 260, 84, 21};
    const kit::Rect btn_joint_war{387, 283, 174, 21};
    const kit::Rect btn_brain{352, 395, 116, 25};
    const kit::Rect btn_back{476, 395, 92, 25};

    std::vector<int> RivalList(const w2::Game& g, int me) const {
        std::vector<int> out;
        for (int i = 0; i < 8; i++) {
            if (i != me && Playing(g, i)) out.push_back(i);
        }
        return out;
    }

    void draw() override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        if (!Playing(g, other) || other == me) {
            auto rv = RivalList(g, me);
            if (!rv.empty()) other = rv[0];
        }
        const w2::Side& o_side = g.map->sides[other];
        const w2::Side& my_side = g.map->sides[me];
        const auto& prof = w2::ai::personality::forSide(o_side);
        auto att = dip::computeAttitude(g, other, me);

        kit::popup(box);
        const Font& f_big = kit::font(2);
        const Font& f_sm = kit::font(0);

        // Top Title & Clickable Rival Switcher Shields
        gfx::setColor(1, 1, 1);
        f_big.colours(7, 0).draw("DIPLOMATIC PARLEY", 74, 50);

        auto rivals = RivalList(g, me);
        int sh_start_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            int r_side = rivals[idx];
            int sx = sh_start_x + static_cast<int>(idx) * 24;
            if (r_side == other) {
                kit::setPal(7);
                gfx::rectangle(gfx::FILL, sx - 2, 49, 20, 20);
                kit::setPal(0);
                kit::outline(sx - 3, 48, 22, 22);
            }
            SmallShield(r_side, sx, 51);
        }

        // Ruler Bio & Stance Banner (72, 73, 496 x 64)
        DrawSunkenPanel(72, 73, 496, 64);
        kit::shield(other, 80, 85);

        std::string arch_tag = "[" + CleanText(prof.archetype) + "]";
        int arch_w = f_sm.width(arch_tag);
        int max_title_w = 432 - arch_w - 10;
        std::string side_label = CleanText(o_side.name, false);
        int side_w = f_big.width(side_label);
        gfx::setColor(1, 1, 1);
        SideBannerFont(f_big, o_side).draw(side_label, 128, 77);
        if (side_w + 16 < max_title_w) {
            std::string ruler_part = FitText(f_big, " - " + prof.name, max_title_w - side_w, false);
            f_big.draw(ruler_part, 128 + side_w, 77);
        }
        kit::right(f_sm.colours(7, 0), arch_tag, 560, 79);

        gfx::setColor(1, 1, 1);
        f_sm.colours(5, 0).draw(FitText(f_sm, "\"" + prof.motto + "\"", 432), 128, 97);

        int st = dip::state(g, me, other);
        bool allied = dip::isAllied(g, me, other);
        std::string st_name =
                allied ? "ALLIED"
                       : (st == dip::PEACE ? "PEACE"
                                           : (st == dip::INTERMEDIATE ? "UNEASY" : "AT WAR"));
        int st_pal = (allied || st == dip::PEACE) ? 10 : (st == dip::INTERMEDIATE ? 7 : 8);
        int tr = dip::truceTurns(g, me, other);
        int att_pal = att.totalScore >= 15 ? 10 : (att.totalScore <= -15 ? 8 : 7);

        int cx = 128;
        cx += f_sm.colours(st_pal, 0).draw("[" + st_name + "]", cx, 115);
        std::string mid_meta = w2::fmt("  Truce:%d  Trust:%+d  Attitude:", tr, att.trustScore);
        cx += f_sm.colours(1, 0).draw(mid_meta, cx, 115);
        std::string att_tail =
                FitText(f_sm, w2::fmt(" %+d (%s)", att.totalScore, att.stanceSummary.c_str()),
                        std::max(20, 560 - cx));
        f_sm.colours(att_pal, 0).draw(att_tail, cx, 115);

        // Left Panel: Attitude Ledger (72, 143, 300 x 167)
        int net_pal = att.totalScore > 0 ? 10 : (att.totalScore < 0 ? 8 : 7);
        DrawSunkenPanel(72, 143, 300, 167, "ATTITUDE LEDGER", w2::fmt("Total: %+d", att.totalScore),
                        net_pal);

        gfx::setScissor(74, 163, 296, 145);
        if (att.factors.empty()) {
            f_sm.colours(1, 0).draw("No active grievances or modifiers.", 82, 172);
        } else {
            int fy = 166;
            for (const auto& fac : att.factors) {
                if (fy > 294) break;
                std::string delta_str = w2::fmt("%+d", fac.delta);
                int d_col = fac.delta > 0 ? 10 : (fac.delta < 0 ? 8 : 1);
                kit::right(f_sm.colours(d_col, 0), delta_str, 110, fy);

                auto f_lines = WrapText(f_sm, fac.label, 246);
                int shown_lines = 0;
                for (const auto& ln : f_lines) {
                    if (shown_lines >= 2 || fy > 296) break;
                    f_sm.draw(ln, 118, fy);
                    fy += 13;
                    shown_lines++;
                }
                fy += 3;
            }
        }
        gfx::setScissor();

        // Right Panel: Treaty & Tribute Controls (380, 143, 188 x 167)
        DrawSunkenPanel(380, 143, 188, 167, "PROPOSE TREATY");

        int my_prop = dip::proposal(g, me, other);
        bool my_ally_prop = dip::proposalAllied(g, me, other);
        int g_off = dip::goldOffer(g, me, other);
        int w_tgt = dip::warTarget(g, me, other);
        std::string w_tgt_name =
                (w_tgt >= 0 && w_tgt < 8 && Playing(g, w_tgt)) ? g.map->sides[w_tgt].name : "None";

        DrawButton(btn_ally.x, btn_ally.y, btn_ally.w, btn_ally.h,
                   my_ally_prop ? "* Propose Alliance *" : "Propose Alliance", my_ally_prop, false,
                   11);
        DrawButton(btn_peace.x, btn_peace.y, btn_peace.w, btn_peace.h,
                   (!my_ally_prop && my_prop == dip::PEACE) ? "* Propose Peace *" : "Propose Peace",
                   !my_ally_prop && my_prop == dip::PEACE, false, 11);
        DrawButton(btn_uneasy.x, btn_uneasy.y, btn_uneasy.w, btn_uneasy.h,
                   my_prop == dip::INTERMEDIATE ? "* Propose Uneasy *" : "Propose Uneasy",
                   my_prop == dip::INTERMEDIATE, false, 13);
        DrawButton(btn_war.x, btn_war.y, btn_war.w, btn_war.h,
                   my_prop == dip::WAR ? "* Propose War *" : "Declare / Propose War",
                   my_prop == dip::WAR, false, 9);

        DrawButton(btn_gold_minus.x, btn_gold_minus.y, btn_gold_minus.w, btn_gold_minus.h,
                   "Gift -50g", false, g_off <= 0, 13);
        DrawButton(btn_gold_plus.x, btn_gold_plus.y, btn_gold_plus.w, btn_gold_plus.h,
                   w2::fmt("Gift +%dg", g_off + 50), g_off > 0, g_off + 50 > my_side.gold, 13);

        DrawButton(btn_joint_war.x, btn_joint_war.y, btn_joint_war.w, btn_joint_war.h,
                   "War Target: " + w_tgt_name, w_tgt != w2::NONE, false, 9);

        // Bottom Panel: Diplomatic Forecast (72, 316, 496 x 72)
        DrawSunkenPanel(72, 316, 496, 72, "DIPLOMATIC FORECAST",
                        w2::fmt("Treasury: %d gp   Gift Offer: %d gp", my_side.gold, g_off), 5);

        gfx::setScissor(74, 336, 492, 50);
        auto wrapped = WrapText(f_sm, att.whatItTakes, 478);
        int wy = 339;
        for (size_t i = 0; i < wrapped.size() && i < 2; i++) {
            f_sm.draw(wrapped[i], 80, wy);
            wy += 14;
        }
        if (allied && dip::alliedTargetCity(g, other) != w2::NONE) {
            if (const w2::City* ac = g.map->city(dip::alliedTargetCity(g, other))) {
                std::string bc = FitText(
                        f_sm,
                        w2::fmt("Allied War Target: %s (%d, %d)", ac->name.c_str(), ac->x, ac->y),
                        478);
                f_sm.colours(10, 0).draw(bc, 80, 369);
            }
        } else if (wy <= 368) {
            int their_prop = dip::proposal(g, other, me);
            bool their_ally = dip::proposalAllied(g, other, me);
            std::string their_str =
                    their_ally ? "Alliance"
                               : (their_prop == dip::PEACE
                                          ? "Peace"
                                          : (their_prop == dip::INTERMEDIATE ? "Uneasy" : "War"));
            f_sm.colours(1, 0).draw(
                    FitText(f_sm, "Their current posture toward you: " + their_str, 478), 80, 369);
        }
        gfx::setScissor();

        // Footer Hints & Action Buttons
        f_sm.colours(1, 0).draw("Left/Right or click top shields to switch", 74, 401);
        DrawButton(btn_brain.x, btn_brain.y, btn_brain.w, btn_brain.h, "AI Brain (F10)");
        DrawButton(btn_back.x, btn_back.y, btn_back.w, btn_back.h, "Back");
    }

    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        auto rivals = RivalList(g, me);
        int sh_start_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            kit::Rect sh{sh_start_x + static_cast<int>(idx) * 24 - 2, 49, 20, 20};
            if (sh.contains(x, y)) {
                other = rivals[idx];
                return;
            }
        }
        if (btn_ally.contains(x, y)) {
            dip::propose(g, me, other, dip::PEACE);
            dip::proposeAlliance(g, me, other, true);
        } else if (btn_peace.contains(x, y)) {
            dip::propose(g, me, other, dip::PEACE);
            dip::proposeAlliance(g, me, other, false);
        } else if (btn_uneasy.contains(x, y)) {
            dip::propose(g, me, other, dip::INTERMEDIATE);
        } else if (btn_war.contains(x, y)) {
            dip::propose(g, me, other, dip::WAR);
            dip::setGoldOffer(g, me, other, 0);
        } else if (btn_gold_minus.contains(x, y)) {
            int cur = dip::goldOffer(g, me, other);
            dip::setGoldOffer(g, me, other, std::max(0, cur - 50));
        } else if (btn_gold_plus.contains(x, y)) {
            int cur = dip::goldOffer(g, me, other);
            if (cur + 50 <= g.map->sides[me].gold) {
                dip::setGoldOffer(g, me, other, cur + 50);
            }
        } else if (btn_joint_war.contains(x, y)) {
            int cur = dip::warTarget(g, me, other);
            int next = w2::NONE;
            int start = (cur == w2::NONE) ? 0 : cur + 1;
            for (int s = start; s < 8; s++) {
                if (s != me && s != other && Playing(g, s)) {
                    next = s;
                    break;
                }
            }
            dip::setWarTarget(g, me, other, next);
        } else if (btn_brain.contains(x, y)) {
            openBrainInspector(other);
        } else if (btn_back.contains(x, y)) {
            bool act = return_to_action;
            kit::pop(this);
            if (act)
                OpenAction();
            else
                OpenReport();
        }
    }

    void keypressed(const std::string& key) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        if (key == "left" || key == "right") {
            auto rivals = RivalList(g, me);
            if (!rivals.empty()) {
                auto it = std::find(rivals.begin(), rivals.end(), other);
                int idx = (it == rivals.end()) ? 0 : static_cast<int>(it - rivals.begin());
                int n = static_cast<int>(rivals.size());
                other = rivals[(idx + (key == "right" ? 1 : n - 1)) % n];
            }
            return;
        }
        if (key == "f10") {
            openBrainInspector(other);
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") {
            bool act = return_to_action;
            kit::pop(this);
            if (act)
                OpenAction();
            else
                OpenReport();
        }
    }
};

// ------------------------------------------------------------------ Turn-Start Envoy Missive Modal
struct MissiveModal : kit::Modal {
    w2::DiplomaticMissive m;
    kit::Done after;

    const kit::Rect box{88, 72, 464, 316};
    const kit::Rect btn_accept{100, 348, 148, 28};
    const kit::Rect btn_parley{258, 348, 136, 28};
    const kit::Rect btn_decline{404, 348, 136, 28};

    void Finish() {
        auto cb = after;
        kit::pop(this);
        pollMissives(cb);
    }

    void draw() override {
        w2::Game& g = *G.g;
        const w2::Side& from = g.map->sides[std::clamp(m.fromSide, 0, 7)];
        kit::popup(box);

        const Font& f_big = kit::font(2);
        const Font& f_sm = kit::font(0);

        // Top Envoy Banner
        DrawSunkenPanel(100, 84, 440, 56);
        kit::shield(m.fromSide, 108, 92);

        gfx::setColor(1, 1, 1);
        SideBannerFont(f_big, from).draw(FitText(f_big, m.headline, 372, false), 156, 90);

        std::string deal_tag = m.proposeAlliance
                                       ? "Proposal: Military Alliance (Shared Vision)"
                                       : (m.proposedState == dip::PEACE
                                                  ? "Proposal: Peace Treaty"
                                                  : (m.proposedState == dip::INTERMEDIATE
                                                             ? "Notice: Border Warning / Uneasy"
                                                             : "Declaration"));
        if (m.goldDemand > 0)
            deal_tag += w2::fmt("  -  Tribute Demand: %d gp", m.goldDemand);
        else if (m.goldDemand < 0)
            deal_tag += w2::fmt("  -  Tribute Gift: +%d gp", -m.goldDemand);
        int deal_pal = (m.proposeAlliance || m.proposedState == dip::PEACE)
                               ? 10
                               : (m.proposedState == dip::INTERMEDIATE ? 7 : 8);
        f_sm.colours(deal_pal, 0).draw(FitText(f_sm, deal_tag, 372), 156, 114);

        // Envoy Message Box
        DrawSunkenPanel(100, 146, 440, 108, "ROYAL ENVOY DISPATCH");
        gfx::setScissor(102, 166, 436, 86);
        auto body_lines = WrapText(f_big, m.body, 420, false);
        int by = 170;
        for (const auto& ln : body_lines) {
            if (by > 236) break;
            f_big.draw(ln, 108, by);
            by += 19;
        }
        gfx::setScissor();

        // Diplomatic Context Box
        DrawSunkenPanel(100, 260, 440, 78, "DIPLOMATIC CONTEXT",
                        w2::fmt("Your Treasury: %d gp", G.player ? G.player->gold : 0), 5);
        gfx::setScissor(102, 280, 436, 56);
        auto ctx_lines = WrapText(f_sm, m.reasonSummary, 420);
        int cy = 283;
        for (const auto& ln : ctx_lines) {
            if (cy > 322) break;
            f_sm.draw(ln, 108, cy);
            cy += 14;
        }
        gfx::setScissor();

        int my_gold = G.player ? G.player->gold : 0;
        bool can_afford = (m.goldDemand <= 0 || my_gold >= m.goldDemand);
        DrawButton(btn_accept.x, btn_accept.y, btn_accept.w, btn_accept.h,
                   m.goldDemand > 0 ? w2::fmt("Pay %dg + Accept", m.goldDemand) : "Accept Proposal",
                   can_afford, !can_afford, 11);
        DrawButton(btn_parley.x, btn_parley.y, btn_parley.w, btn_parley.h, "Open Parley");
        DrawButton(btn_decline.x, btn_decline.y, btn_decline.w, btn_decline.h, "Decline", false,
                   false, 9);
    }

    void mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        if (btn_accept.contains(x, y)) {
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
            if (m.jointWarTarget != w2::NONE && Playing(g, m.jointWarTarget)) {
                g.diplomacy.state[me * 8 + m.jointWarTarget] = dip::WAR;
                g.diplomacy.state[m.jointWarTarget * 8 + me] = dip::WAR;
                dip::propose(g, me, m.jointWarTarget, dip::WAR);
            }
            front::stratDirty();
            Finish();
        } else if (btn_parley.contains(x, y)) {
            int from = m.fromSide;
            kit::pop(this);
            openParley(from, false);
        } else if (btn_decline.contains(x, y)) {
            Finish();
        }
    }

    void keypressed(const std::string& key) override {
        if (key == "escape")
            Finish();
        else if (key == "return" || key == "kpenter") {
            mousepressed(btn_accept.x + 5, btn_accept.y + 5, 1);
        }
    }
};

// ------------------------------------------------------------------ F10 AI Brain & Policy
// Inspector
struct BrainInspectorModal : kit::Modal {
    int side_idx = 0;
    const kit::Rect box{44, 34, 552, 412};
    const kit::Rect btn_toggle_engine{380, 71, 128, 19};
    const kit::Rect btn_close{514, 71, 64, 19};

    void draw() override {
        w2::Game& g = *G.g;
        if (!Playing(g, side_idx)) {
            for (int s = 0; s < 8; s++) {
                if (Playing(g, s) && g.map->sides[s].computer) {
                    side_idx = s;
                    break;
                }
            }
            if (!Playing(g, side_idx)) side_idx = ActiveViewerIndex(g);
        }
        w2::Side& s = g.map->sides[side_idx];
        const auto& prof = w2::ai::personality::forSide(s);
        const auto& b = w2::ai::belief::ensure(s);

        kit::popup(box);
        const Font& f_big = kit::font(2);
        const Font& f_sm = kit::font(0);

        gfx::setColor(1, 1, 1);
        f_big.colours(7, 0).draw("AI BRAIN INSPECTOR (F10)", 56, 42);

        // Top row of clickable faction shields
        for (int i = 0; i < 8; i++) {
            if (!Playing(g, i)) continue;
            int sx = 376 + i * 26;
            if (i == side_idx) {
                kit::setPal(7);
                gfx::rectangle(gfx::FILL, sx - 2, 41, 20, 20);
                kit::setPal(0);
                kit::outline(sx - 3, 40, 22, 22);
            }
            SmallShield(i, sx, 43);
        }

        // Faction Bio & Value Head Summary Banner (56, 67, 528 x 58)
        DrawSunkenPanel(56, 67, 528, 58);
        kit::shield(side_idx, 64, 76);

        std::string side_title = CleanText(s.name, false);
        int st_w = f_big.width(side_title);
        gfx::setColor(1, 1, 1);
        SideBannerFont(f_big, s).draw(FitText(f_big, side_title, 260, false), 112, 71);
        if (st_w + 16 < 262) {
            f_big.draw(FitText(f_big, " - " + prof.name, 262 - st_w, false), 112 + st_w, 71);
        }
        f_sm.colours(5, 0).draw(FitText(f_sm, "\"" + prof.motto + "\"", 464), 112, 91);

        int explored_pct = static_cast<int>(std::round((1.0F - b.frontierRatio) * 100.0F));
        std::string eval_str = w2::fmt("[%s]  V(s):%+.2f  Expl:%d%%  Cit:%d  Gold:%d gp",
                                       prof.archetype.c_str(), s.lastBoardValue, explored_pct,
                                       static_cast<int>(b.visibleCities.size()), s.gold);
        int val_pal = s.lastBoardValue >= 0.05F ? 10 : (s.lastBoardValue <= -0.05F ? 8 : 7);
        f_sm.colours(val_pal, 0).draw(FitText(f_sm, eval_str, 464), 112, 106);

        DrawButton(btn_toggle_engine.x, btn_toggle_engine.y, btn_toggle_engine.w,
                   btn_toggle_engine.h, s.classicAi ? "AI: 1993 Classic" : "AI: Learnable RL",
                   !s.classicAi, false, 11);
        DrawButton(btn_close.x, btn_close.y, btn_close.w, btn_close.h, "Close");

        // Left Panel: 10D Personality Vector Trait Bars (56, 130, 212 x 165)
        DrawSunkenPanel(56, 130, 212, 165, "10D PERSONALITY TRAITS");
        static const char* const kTraitLabels[10] = {
            "Aggression", "Honour",  "Avarice", "Scorched", "HeroFocus",
            "Coalition",  "Cavalry", "Fliers",  "Siege",    "Caution"};
        for (int t = 0; t < 10; t++) {
            int ty = 150 + t * 14;
            f_sm.draw(kTraitLabels[t], 64, ty);
            int bar_x = 160, bar_w = 98, mid_x = bar_x + bar_w / 2;
            kit::setPal(3);
            gfx::rectangle(gfx::FILL, bar_x, ty + 3, bar_w, 8);
            kit::bevel(bar_x, ty + 3, bar_w, 8, 0, 2);
            float val = std::clamp(prof.traits[t], -1.0F, 1.0F);
            int fill_w = static_cast<int>(std::round(std::abs(val) * (bar_w / 2 - 2)));
            kit::setPal(val >= 0 ? 10 : 8);
            if (fill_w > 0) {
                if (val >= 0)
                    gfx::rectangle(gfx::FILL, mid_x + 1, ty + 4, fill_w, 6);
                else
                    gfx::rectangle(gfx::FILL, mid_x - fill_w, ty + 4, fill_w, 6);
            }
            kit::setPal(15);
            gfx::rectangle(gfx::FILL, mid_x, ty + 2, 1, 10);
        }

        // Right Panel: Fair-Information Belief Summary (276, 130, 308 x 165)
        DrawSunkenPanel(276, 130, 308, 165, "BELIEF MEMORY",
                        w2::fmt("Fog:%d Stacks:%d", static_cast<int>(b.fogFrontiers.size()),
                                static_cast<int>(b.stacksByTile.size())),
                        5);
        gfx::setScissor(278, 150, 304, 143);
        f_sm.colours(1, 0).draw("Rival", 284, 151);
        kit::right(f_sm.colours(1, 0), "Cit", 434, 151);
        kit::right(f_sm.colours(1, 0), "Str", 478, 151);
        kit::right(f_sm.colours(1, 0), "Us / Them", 574, 151);
        kit::setPal(0);
        gfx::rectangle(gfx::FILL, 282, 165, 296, 1);

        int rby = 169;
        for (int o = 0; o < 8; o++) {
            if (o == side_idx || !Playing(g, o)) continue;
            if (rby > 279) break;
            SmallShield(o, 284, rby - 1);
            std::string r_name = FitText(f_sm, g.map->sides[o].name, 106);
            f_sm.draw(r_name, 304, rby);
            kit::right(f_sm.colours(7, 0), w2::fmt("%d", b.visibleCitiesBySide[o]), 434, rby);
            kit::right(f_sm.colours(5, 0), w2::fmt("%.1f", b.seenEnemyAvgStrength[o]), 478, rby);
            std::string bdr =
                    w2::fmt("%.0f / %.0f", b.myBorderStrength[o], b.theirBorderStrength[o]);
            int b_pal = b.myBorderStrength[o] >= b.theirBorderStrength[o] ? 10 : 8;
            kit::right(f_sm.colours(b_pal, 0), bdr, 574, rby);
            rby += 17;
        }
        gfx::setScissor();

        // Bottom Panel: Top Scored Neural Policy Decisions (56, 301, 528 x 135)
        DrawSunkenPanel(56, 301, 528, 135, "RECENT NEURAL POLICY DECISIONS",
                        "Left/Right: Switch Side", 1);
        gfx::setScissor(58, 321, 524, 113);
        if (s.aiThoughts.empty()) {
            f_sm.colours(1, 0).draw(
                    "No neural thoughts recorded yet (advance a turn to watch this AI act).", 66,
                    330);
        } else {
            int ty = 323;
            int shown = 0;
            for (int pass = 0; pass < 2; pass++) {
                for (const auto& th : s.aiThoughts) {
                    bool is_diplo = (th.category == "Diplomacy");
                    if ((pass == 0 && is_diplo) || (pass == 1 && !is_diplo)) continue;
                    if (shown >= 7 || ty > 420) break;
                    int cat_pal = 15;
                    if (th.stageNear)
                        cat_pal = 7;
                    else if (th.category == "Economy")
                        cat_pal = 5;
                    else if (th.category == "Spoils")
                        cat_pal = 8;
                    else if (th.category == "Diplomacy")
                        cat_pal = 10;

                    std::string short_cat =
                            th.category == "Mission"
                                    ? "Move"
                                    : (th.category == "Economy"
                                               ? "Econ"
                                               : (th.category == "Diplomacy" ? "Diplo"
                                                                             : th.category));
                    std::string actor_compact = th.actorName;
                    size_t u_pos = actor_compact.find("-unit stack");
                    if (u_pos != std::string::npos) actor_compact.replace(u_pos, 11, "u");
                    std::string tag = "[" + short_cat + "] ";
                    int tag_w = f_sm.colours(cat_pal, 0).draw(tag, 64, ty);
                    std::string rest =
                            FitText(f_sm, actor_compact + " -> " + th.targetName + ": " + th.detail,
                                    512 - tag_w);
                    f_sm.draw(rest, 64 + tag_w, ty);
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
            if (!Playing(g, i)) continue;
            kit::Rect sh{374 + i * 26, 41, 20, 20};
            if (sh.contains(x, y)) {
                side_idx = i;
                return;
            }
        }
        if (btn_toggle_engine.contains(x, y)) {
            g.map->sides[side_idx].classicAi = !g.map->sides[side_idx].classicAi;
            return;
        }
        if (btn_close.contains(x, y)) {
            kit::pop(this);
        }
    }

    void keypressed(const std::string& key) override {
        w2::Game& g = *G.g;
        if (key == "left" || key == "right") {
            int dir = (key == "right") ? 1 : 7;
            for (int step = 1; step <= 8; step++) {
                int cand = (side_idx + dir * step) % 8;
                if (Playing(g, cand)) {
                    side_idx = cand;
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
        OpenReport();
    else
        OpenClassicReport();
}

void action() {
    if (G.g->map->options.diplomacy == 0) return;
    if (G.modernUi)
        OpenAction();
    else
        OpenClassicAction();
}

void openParley(int other_side, bool return_to_action) {
    if (!G.modernUi || !G.g || other_side < 0 || other_side >= 8) return;
    auto d = std::make_shared<ParleyModal>();
    d->other = other_side;
    d->return_to_action = return_to_action;
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
    if (!Playing(*G.g, m.fromSide)) {
        pollMissives(after);
        return;
    }
    auto d = std::make_shared<MissiveModal>();
    d->m = std::move(m);
    d->after = after;
    kit::push(d);
}

void openBrainInspector(int initial_side) {
    if (!G.modernUi || !G.g) return;
    auto d = std::make_shared<BrainInspectorModal>();
    if (initial_side >= 0 && initial_side < 8 && Playing(*G.g, initial_side)) {
        d->side_idx = initial_side;
    } else {
        for (int i = 0; i < 8; i++) {
            if (Playing(*G.g, i) && G.g->map->sides[i].computer) {
                d->side_idx = i;
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
