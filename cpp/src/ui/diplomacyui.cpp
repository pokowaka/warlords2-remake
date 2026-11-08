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
    return i >= 0 && i < 8 && g.map->sides[i].in_use && g.map->sides[i].alive;
}

int ActiveViewerIndex(const w2::Game& g) {
    if (G.player && G.player->index >= 0 && G.player->index < 8) return G.player->index;
    for (int i = 0; i < 8; i++) {
        if (Playing(g, i)) return i;
    }
    return 0;
}

void Diplom(int sx, int sy, int w, int h, int x, int y) {
    if (auto art = G.screen->ArtFor(47)) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(art->image, gfx::NewQuad(sx, sy, w, h), x, y);
    }
}

void SmallShield(int side, int x, int y) {
    gfx::SetColor(1, 1, 1);
    gfx::Draw(G.shields_img, gfx::NewQuad(side * 40 + 24, 46, 16, 16), x, y);
}

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

struct ClassicReport : kit::Modal {
    screen::View v;
    std::map<int, std::string> titles;
    std::vector<w2::Side*> order;
    void Draw() override {
        w2::Game& g = *G.g;
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6b, 0), 320, 64);
        kit::SetPal(1);
        gfx::Rectangle(gfx::kFill, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 120 + 32 * i, 110, 1, 261);
        gfx::Rectangle(gfx::kFill, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 88, 139 + 29 * i, 288, 1);
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
                    int st = dip::State(g, col, row);
                    if (st == dip::kWar)
                        Diplom(384, 0, 32, 29, x, y);
                    else if (st == dip::kPeace)
                        Diplom(320, 0, 32, 29, x, y);
                }
            }
        }
        kit::SetPal(0);
        kit::Outline(392, 110, 160, 262);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(f, kit::Text(0x6d, 0), 472, 116);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(order[i]->index, 400, y);
            gfx::SetColor(1, 1, 1);
            f.Draw(titles[order[i]->index], 432, y);
        }
        kit::DrawControls(v, hidden);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c) return;
        if (c->id == kDone)
            kit::Pop(this);
        else if (c->id == kAction) {
            kit::Pop(this);
            OpenClassicAction();
        }
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

void OpenClassicReport() {
    auto d = std::make_shared<ClassicReport>();
    d->v = kit::View(kReport);
    d->view = &d->v;
    d->v.state[kDone] = d->v.state[kAction] = w2::uidata::kNormal;
    if (!w2::game::HasActiveHuman(*G.g)) d->hidden.insert(kAction);
    d->titles = dip::Ratings(*G.g);
    for (w2::Side* s : G.g->sides)
        if (s->alive) d->order.push_back(s);
    w2::LuaSort(d->order, [](w2::Side* p, w2::Side* q) {
        if (p->diplo_score != q->diplo_score) return p->diplo_score < q->diplo_score;
        return p->index < q->index;
    });
    kit::Push(d);
}

struct ClassicAction : kit::Modal {
    screen::View v;
    std::vector<int> others;
    void Draw() override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        const w2::Side& my_side = g.map->sides[me];
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6c, 0), 320, 64);
        kit::Shield(me, 96, 111);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        f.Draw(my_side.name, 144, 122);
        const int ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) f.Draw(kit::Text(0x6e, i), 104, ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::Shield(other, x, 111);
            if (!Playing(g, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(x, y);
            } else {
                int st = dip::State(g, other, me);
                Diplom(80 + 120 * st, 45, 40, 40, x, 151);
                int theirs = dip::Proposal(g, other, me);
                if (theirs == st)
                    Blank(x, 191);
                else
                    Diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                int mine = dip::Proposal(g, me, other);
                for (int k = 0; k <= 2; k++)
                    Diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
            }
        }
        std::set<int> h;
        for (int k = 0; k <= 2; k++)
            for (int i = 0; i <= 6; i++) h.insert(kOffer[k] + i);
        kit::DrawControls(v, h);
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        if (c->id == kOk) {
            kit::Pop(this);
            return;
        }
        if (c->id == kBack) {
            kit::Pop(this);
            OpenClassicReport();
            return;
        }
        int me = ActiveViewerIndex(*G.g);
        for (int k = 0; k <= 2; k++) {
            int i = c->id - kOffer[k];
            if (i >= 0 && i < 7 && i < static_cast<int>(others.size()))
                dip::Propose(*G.g, me, others[i], k);
        }
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

void OpenClassicAction() {
    auto d = std::make_shared<ClassicAction>();
    d->v = kit::View(kAct);
    d->view = &d->v;
    for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::kNormal;
    int me = ActiveViewerIndex(*G.g);
    for (int i = 0; i < 8; i++)
        if (i != me) d->others.push_back(i);
    kit::Push(d);
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
    if (f.Width(s) <= max_w) return s;
    const std::string ell = "..";
    int ell_w = f.Width(ell);
    if (ell_w >= max_w) return "";
    while (!s.empty() && f.Width(s) + ell_w > max_w) {
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
        if (f.Width(word) > max_w) {
            word = FitText(f, word, max_w, false);
        }
        std::string test = cur.empty() ? word : cur + " " + word;
        if (f.Width(test) <= max_w || cur.empty()) {
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
    kit::SetPal(4);  // deep charcoal interior (31,31,31)
    gfx::Rectangle(gfx::kFill, x, y, w, h);
    kit::Bevel(x, y, w, h, 0, 1);  // sunken 3D stone bevel: black top/left, silver bottom/right
    if (!left_hdr.empty() || !right_hdr.empty()) {
        kit::SetPal(3);  // subtle slate header band (44,44,44)
        gfx::Rectangle(gfx::kFill, x + 1, y + 1, w - 2, 17);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, x + 1, y + 18, w - 2, 1);
        const Font& f_sm = kit::GetFont(0);
        gfx::SetColor(1, 1, 1);
        int avail_w = w - 14;
        std::string r_clean = right_hdr.empty() ? "" : FitText(f_sm, right_hdr, avail_w / 2 + 36);
        int r_w = r_clean.empty() ? 0 : f_sm.Width(r_clean) + 10;
        if (!left_hdr.empty()) {
            std::string l_clean = FitText(f_sm, left_hdr, std::max(24, avail_w - r_w));
            f_sm.Colours(7, 0).Draw(l_clean, x + 7, y + 3);
        }
        if (!r_clean.empty()) {
            kit::Right(f_sm.Colours(right_pal, 0), r_clean, x + w - 7, y + 3);
        }
    }
}

// Raised 3D stone button with crisp black-outlined text and semantic active fill.
void DrawButton(int x, int y, int w, int h, const std::string& label, bool active = false,
                bool disabled = false, int accent_pal = 11) {
    kit::SetPal(0);
    kit::Outline(x, y, w, h);
    kit::SetPal(disabled ? 3 : (active ? accent_pal : 2));
    gfx::Rectangle(gfx::kFill, x + 1, y + 1, w - 2, h - 2);
    kit::Bevel(x + 1, y + 1, w - 2, h - 2, active ? 7 : (disabled ? 2 : 1), 0);

    const Font& f_sm = kit::GetFont(0);
    int glyph = disabled ? 2 : (active ? 7 : 15);
    const Font& f = f_sm.Colours(glyph, 0);
    std::string fitted = FitText(f_sm, label, w - 8);
    gfx::SetColor(1, 1, 1);
    kit::Centred(f, fitted, x + w / 2, y + (h - f.line_height) / 2);
}

// High-contrast heraldic font for dark slate panels (lifts Lich King black/navy to crisp readable
// tones).
const Font& SideBannerFont(const Font& f_big, const w2::Side& s) {
    if (s.colour == 0) return f_big.Colours(15, s.edge == 0 ? 9 : s.edge);
    if (s.colour == 6) return f_big.Colours(5, 0);
    return f_big.Colours(s.colour, s.edge);
}

void OpenReport();
void OpenAction();

struct Report : kit::Modal {
    screen::View v;
    std::map<int, std::string> titles;
    std::vector<w2::Side*> order;
    void Draw() override {
        w2::Game& g = *G.g;
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6b, 0), 320, 64);
        kit::SetPal(1);
        gfx::Rectangle(gfx::kFill, 88, 139, 1, 232);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 120 + 32 * i, 110, 1, 261);
        gfx::Rectangle(gfx::kFill, 120, 110, 256, 1);
        for (int i = 0; i <= 8; i++) gfx::Rectangle(gfx::kFill, 88, 139 + 29 * i, 288, 1);
        for (int i = 0; i < 8; i++) {
            if (Playing(g, i)) {
                SmallShield(i, 128 + 32 * i, 117);
                SmallShield(i, 96, 146 + 29 * i);
            }
        }
        const Font& tf = kit::GetFont(0);
        for (int col = 0; col < 8; col++) {
            for (int row = 0; row < 8; row++) {
                int x = 120 + 32 * col, y = 139 + 29 * row;
                if (col == row)
                    Diplom(384, 29, 32, 29, x, y);
                else if (Playing(g, col) && Playing(g, row)) {
                    int st = dip::State(g, col, row);
                    if (st == dip::kWar)
                        Diplom(384, 0, 32, 29, x, y);
                    else if (st == dip::kPeace) {
                        Diplom(320, 0, 32, 29, x, y);
                        if (dip::IsAllied(g, col, row)) {
                            kit::SetPal(10);
                            kit::Outline(x + 1, y + 1, 30, 27);
                            gfx::SetColor(1, 1, 1);
                            tf.Colours(7, 0).Draw("ALLY", x + 3, y + 2);
                        }
                    }
                }
            }
        }
        // Recessed ratings panel on the right
        kit::SetPal(4);
        gfx::Rectangle(gfx::kFill, 392, 110, 160, 262);
        kit::Bevel(392, 110, 160, 262, 0, 1);
        const Font& f = kit::GetFont(2);
        gfx::SetColor(1, 1, 1);
        kit::Centred(f.Colours(7, 0), kit::Text(0x6d, 0), 472, 116);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 394, 138, 156, 1);
        for (size_t i = 0; i < order.size(); i++) {
            int y = 146 + 29 * static_cast<int>(i);
            SmallShield(order[i]->index, 400, y);
            gfx::SetColor(1, 1, 1);
            std::string r_title = FitText(f, titles[order[i]->index], 120, false);
            f.Draw(r_title, 424, y);
        }
        // Centered cleanly between ACTION (96..160) and DONE (480..544)
        gfx::SetColor(1, 1, 1);
        kit::Centred(tf.Colours(7, 0), "Click any shield to Parley  -  F10: AI Brain", 320, 384);
        kit::DrawControls(v);
    }
    void Mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        for (int i = 0; i < 8; i++) {
            if (i == me || !Playing(g, i)) continue;
            kit::Rect top_sh{120 + 32 * i, 110, 32, 28};
            kit::Rect left_sh{88, 139 + 29 * i, 32, 29};
            if (top_sh.Contains(x, y) || left_sh.Contains(x, y)) {
                kit::Pop(this);
                OpenParley(i, false);
                return;
            }
        }
        for (size_t i = 0; i < order.size(); i++) {
            int s_idx = order[i]->index;
            if (s_idx == me) continue;
            kit::Rect rate_row{394, 142 + 29 * static_cast<int>(i), 156, 26};
            if (rate_row.Contains(x, y)) {
                kit::Pop(this);
                OpenParley(s_idx, false);
                return;
            }
        }
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        if (c->id == kDone)
            kit::Pop(this);
        else if (c->id == kAction) {
            kit::Pop(this);
            OpenAction();
        }
    }
    void Keypressed(const std::string& key) override {
        if (key == "f10") {
            OpenBrainInspector();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

void OpenReport() {
    auto d = std::make_shared<Report>();
    d->v = kit::View(kReport);
    d->view = &d->v;
    d->v.state[kDone] = d->v.state[kAction] = w2::uidata::kNormal;
    d->titles = dip::Ratings(*G.g);
    for (w2::Side* s : G.g->sides)
        if (s->alive) d->order.push_back(s);
    w2::LuaSort(d->order, [](w2::Side* p, w2::Side* q) {
        if (p->diplo_score != q->diplo_score) return p->diplo_score < q->diplo_score;
        return p->index < q->index;
    });
    kit::Push(d);
}

struct Action : kit::Modal {
    screen::View v;
    std::vector<int> others;
    void Draw() override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        const w2::Side& my_side = g.map->sides[me];
        kit::Popup(kR);
        gfx::SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(1), kit::Text(0x6c, 0), 320, 64);
        kit::Shield(me, 96, 111);
        const Font& f = kit::GetFont(2);
        const Font& tf = kit::GetFont(0);
        gfx::SetColor(1, 1, 1);
        SideBannerFont(f, my_side).Draw(FitText(f, my_side.name, 122, false), 144, 122);
        const int ys[7] = {165, 205, 251, 271, 291, 311, 331};
        for (int i = 0; i < 7; i++) f.Draw(kit::Text(0x6e, i), 104, ys[i]);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            int x = 272 + 40 * static_cast<int>(col);
            kit::Shield(other, x, 111);
            if (!Playing(g, other)) {
                for (int y : {151, 191, 245, 285, 325}) Blank(x, y);
            } else {
                int st = dip::State(g, other, me);
                Diplom(80 + 120 * st, 45, 40, 40, x, 151);
                if (dip::IsAllied(g, other, me)) {
                    kit::SetPal(10);
                    kit::Outline(x + 1, 152, 38, 38);
                    gfx::SetColor(1, 1, 1);
                    tf.Colours(7, 0).Draw("ALLY", x + 6, 153);
                }
                int tr = dip::TruceTurns(g, me, other);
                if (tr > 0) {
                    gfx::SetColor(1, 1, 1);
                    tf.Colours(7, 0).Draw(w2::Fmt("T:%d", tr), x + 8, 177);
                }

                int theirs = dip::Proposal(g, other, me);
                bool their_ally = dip::ProposalAllied(g, other, me);
                if (theirs == st && (!their_ally || dip::IsAllied(g, other, me)))
                    Blank(x, 191);
                else {
                    Diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
                    if (their_ally) {
                        gfx::SetColor(1, 1, 1);
                        tf.Colours(7, 0).Draw("ALLY", x + 6, 193);
                    }
                }

                int mine = dip::Proposal(g, me, other);
                bool my_ally = dip::ProposalAllied(g, me, other);
                for (int k = 0; k <= 2; k++) {
                    Diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
                }
                if (my_ally && mine == dip::kPeace) {
                    kit::SetPal(10);
                    kit::Outline(x + 1, 246, 38, 38);
                    gfx::SetColor(1, 1, 1);
                    tf.Colours(7, 0).Draw("ALLY", x + 6, 247);
                }
                int g_off = dip::GoldOffer(g, me, other);
                if (g_off > 0) {
                    gfx::SetColor(1, 1, 1);
                    tf.Colours(7, 0).Draw(w2::Fmt("+%dg", g_off), x + 3, 271);
                }
            }
        }
        // Centered cleanly between BACK (96..160) and OK (480..544)
        gfx::SetColor(1, 1, 1);
        kit::Centred(tf.Colours(7, 0), "Click a top shield to Parley  -  F10: AI Brain", 320, 384);
        std::set<int> h;
        for (int k = 0; k <= 2; k++)
            for (int i = 0; i <= 6; i++) h.insert(kOffer[k] + i);
        kit::DrawControls(v, h);
    }
    void Mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        for (size_t col = 0; col < others.size(); col++) {
            int other = others[col];
            if (!Playing(g, other)) continue;
            kit::Rect sh_rect{272 + 40 * static_cast<int>(col), 108, 40, 43};
            if (sh_rect.Contains(x, y)) {
                kit::Pop(this);
                OpenParley(other, true);
                return;
            }
        }
        auto c = kit::ControlAt(v, x, y);
        if (!c) return;
        if (c->id == kOk) {
            kit::Pop(this);
            return;
        }
        if (c->id == kBack) {
            kit::Pop(this);
            OpenReport();
            return;
        }
        for (int k = 0; k <= 2; k++) {
            int i = c->id - kOffer[k];
            if (i >= 0 && i < 7 && i < static_cast<int>(others.size())) {
                int other = others[i];
                if (k == dip::kPeace && dip::Proposal(g, me, other) == dip::kPeace) {
                    bool next_ally = !dip::ProposalAllied(g, me, other);
                    dip::ProposeAlliance(g, me, other, next_ally);
                } else {
                    dip::Propose(g, me, other, k);
                }
            }
        }
    }
    void Keypressed(const std::string& key) override {
        if (key == "f10") {
            OpenBrainInspector();
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") kit::Pop(this);
    }
};

void OpenAction() {
    auto d = std::make_shared<Action>();
    d->v = kit::View(kAct);
    d->view = &d->v;
    int me = ActiveViewerIndex(*G.g);
    for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::kNormal;
    for (int i = 0; i < 8; i++)
        if (i != me) d->others.push_back(i);
    kit::Push(d);
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

    void Draw() override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        if (!Playing(g, other) || other == me) {
            auto rv = RivalList(g, me);
            if (!rv.empty()) other = rv[0];
        }
        const w2::Side& o_side = g.map->sides[other];
        const w2::Side& my_side = g.map->sides[me];
        const auto& prof = w2::ai::personality::ForSide(o_side);
        auto att = dip::ComputeAttitude(g, other, me);

        kit::Popup(box);
        const Font& f_big = kit::GetFont(2);
        const Font& f_sm = kit::GetFont(0);

        // Top Title & Clickable Rival Switcher Shields
        gfx::SetColor(1, 1, 1);
        f_big.Colours(7, 0).Draw("DIPLOMATIC PARLEY", 74, 50);

        auto rivals = RivalList(g, me);
        int sh_start_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            int r_side = rivals[idx];
            int sx = sh_start_x + static_cast<int>(idx) * 24;
            if (r_side == other) {
                kit::SetPal(7);
                gfx::Rectangle(gfx::kFill, sx - 2, 49, 20, 20);
                kit::SetPal(0);
                kit::Outline(sx - 3, 48, 22, 22);
            }
            SmallShield(r_side, sx, 51);
        }

        // Ruler Bio & Stance Banner (72, 73, 496 x 64)
        DrawSunkenPanel(72, 73, 496, 64);
        kit::Shield(other, 80, 85);

        std::string arch_tag = "[" + CleanText(prof.archetype) + "]";
        int arch_w = f_sm.Width(arch_tag);
        int max_title_w = 432 - arch_w - 10;
        std::string side_label = CleanText(o_side.name, false);
        int side_w = f_big.Width(side_label);
        gfx::SetColor(1, 1, 1);
        SideBannerFont(f_big, o_side).Draw(side_label, 128, 77);
        if (side_w + 16 < max_title_w) {
            std::string ruler_part = FitText(f_big, " - " + prof.name, max_title_w - side_w, false);
            f_big.Draw(ruler_part, 128 + side_w, 77);
        }
        kit::Right(f_sm.Colours(7, 0), arch_tag, 560, 79);

        gfx::SetColor(1, 1, 1);
        f_sm.Colours(5, 0).Draw(FitText(f_sm, "\"" + prof.motto + "\"", 432), 128, 97);

        int st = dip::State(g, me, other);
        bool allied = dip::IsAllied(g, me, other);
        std::string st_name =
                allied ? "ALLIED"
                       : (st == dip::kPeace ? "PEACE"
                                            : (st == dip::kIntermediate ? "UNEASY" : "AT WAR"));
        int st_pal = (allied || st == dip::kPeace) ? 10 : (st == dip::kIntermediate ? 7 : 8);
        int tr = dip::TruceTurns(g, me, other);
        int att_pal = att.total_score >= 15 ? 10 : (att.total_score <= -15 ? 8 : 7);

        int cx = 128;
        cx += f_sm.Colours(st_pal, 0).Draw("[" + st_name + "]", cx, 115);
        std::string mid_meta = w2::Fmt("  Truce:%d  Trust:%+d  Attitude:", tr, att.trust_score);
        cx += f_sm.Colours(1, 0).Draw(mid_meta, cx, 115);
        std::string att_tail =
                FitText(f_sm, w2::Fmt(" %+d (%s)", att.total_score, att.stance_summary.c_str()),
                        std::max(20, 560 - cx));
        f_sm.Colours(att_pal, 0).Draw(att_tail, cx, 115);

        // Left Panel: Attitude Ledger (72, 143, 300 x 167)
        int net_pal = att.total_score > 0 ? 10 : (att.total_score < 0 ? 8 : 7);
        DrawSunkenPanel(72, 143, 300, 167, "ATTITUDE LEDGER",
                        w2::Fmt("Total: %+d", att.total_score), net_pal);

        gfx::SetScissor(74, 163, 296, 145);
        if (att.factors.empty()) {
            f_sm.Colours(1, 0).Draw("No active grievances or modifiers.", 82, 172);
        } else {
            int fy = 166;
            for (const auto& fac : att.factors) {
                if (fy > 294) break;
                std::string delta_str = w2::Fmt("%+d", fac.delta);
                int d_col = fac.delta > 0 ? 10 : (fac.delta < 0 ? 8 : 1);
                kit::Right(f_sm.Colours(d_col, 0), delta_str, 110, fy);

                auto f_lines = WrapText(f_sm, fac.label, 246);
                int shown_lines = 0;
                for (const auto& ln : f_lines) {
                    if (shown_lines >= 2 || fy > 296) break;
                    f_sm.Draw(ln, 118, fy);
                    fy += 13;
                    shown_lines++;
                }
                fy += 3;
            }
        }
        gfx::SetScissor();

        // Right Panel: Treaty & Tribute Controls (380, 143, 188 x 167)
        DrawSunkenPanel(380, 143, 188, 167, "PROPOSE TREATY");

        int my_prop = dip::Proposal(g, me, other);
        bool my_ally_prop = dip::ProposalAllied(g, me, other);
        int g_off = dip::GoldOffer(g, me, other);
        int w_tgt = dip::WarTarget(g, me, other);
        std::string w_tgt_name =
                (w_tgt >= 0 && w_tgt < 8 && Playing(g, w_tgt)) ? g.map->sides[w_tgt].name : "None";

        DrawButton(btn_ally.x, btn_ally.y, btn_ally.w, btn_ally.h,
                   my_ally_prop ? "* Propose Alliance *" : "Propose Alliance", my_ally_prop, false,
                   11);
        DrawButton(
                btn_peace.x, btn_peace.y, btn_peace.w, btn_peace.h,
                (!my_ally_prop && my_prop == dip::kPeace) ? "* Propose Peace *" : "Propose Peace",
                !my_ally_prop && my_prop == dip::kPeace, false, 11);
        DrawButton(btn_uneasy.x, btn_uneasy.y, btn_uneasy.w, btn_uneasy.h,
                   my_prop == dip::kIntermediate ? "* Propose Uneasy *" : "Propose Uneasy",
                   my_prop == dip::kIntermediate, false, 13);
        DrawButton(btn_war.x, btn_war.y, btn_war.w, btn_war.h,
                   my_prop == dip::kWar ? "* Propose War *" : "Declare / Propose War",
                   my_prop == dip::kWar, false, 9);

        DrawButton(btn_gold_minus.x, btn_gold_minus.y, btn_gold_minus.w, btn_gold_minus.h,
                   "Gift -50g", false, g_off <= 0, 13);
        DrawButton(btn_gold_plus.x, btn_gold_plus.y, btn_gold_plus.w, btn_gold_plus.h,
                   w2::Fmt("Gift +%dg", g_off + 50), g_off > 0, g_off + 50 > my_side.gold, 13);

        DrawButton(btn_joint_war.x, btn_joint_war.y, btn_joint_war.w, btn_joint_war.h,
                   "War Target: " + w_tgt_name, w_tgt != w2::kNone, false, 9);

        // Bottom Panel: Diplomatic Forecast (72, 316, 496 x 72)
        DrawSunkenPanel(72, 316, 496, 72, "DIPLOMATIC FORECAST",
                        w2::Fmt("Treasury: %d gp   Gift Offer: %d gp", my_side.gold, g_off), 5);

        gfx::SetScissor(74, 336, 492, 50);
        auto wrapped = WrapText(f_sm, att.what_it_takes, 478);
        int wy = 339;
        for (size_t i = 0; i < wrapped.size() && i < 2; i++) {
            f_sm.Draw(wrapped[i], 80, wy);
            wy += 14;
        }
        if (allied && dip::AlliedTargetCity(g, other) != w2::kNone) {
            if (const w2::City* ac = g.map->City(dip::AlliedTargetCity(g, other))) {
                std::string bc = FitText(
                        f_sm,
                        w2::Fmt("Allied War Target: %s (%d, %d)", ac->name.c_str(), ac->x, ac->y),
                        478);
                f_sm.Colours(10, 0).Draw(bc, 80, 369);
            }
        } else if (wy <= 368) {
            int their_prop = dip::Proposal(g, other, me);
            bool their_ally = dip::ProposalAllied(g, other, me);
            std::string their_str =
                    their_ally ? "Alliance"
                               : (their_prop == dip::kPeace
                                          ? "Peace"
                                          : (their_prop == dip::kIntermediate ? "Uneasy" : "War"));
            f_sm.Colours(1, 0).Draw(
                    FitText(f_sm, "Their current posture toward you: " + their_str, 478), 80, 369);
        }
        gfx::SetScissor();

        // Footer Hints & Action Buttons
        f_sm.Colours(1, 0).Draw("Left/Right or click top shields to switch", 74, 401);
        DrawButton(btn_brain.x, btn_brain.y, btn_brain.w, btn_brain.h, "AI Brain (F10)");
        DrawButton(btn_back.x, btn_back.y, btn_back.w, btn_back.h, "Back");
    }

    void Mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        auto rivals = RivalList(g, me);
        int sh_start_x = 568 - static_cast<int>(rivals.size()) * 24;
        for (size_t idx = 0; idx < rivals.size(); idx++) {
            kit::Rect sh{sh_start_x + static_cast<int>(idx) * 24 - 2, 49, 20, 20};
            if (sh.Contains(x, y)) {
                other = rivals[idx];
                return;
            }
        }
        if (btn_ally.Contains(x, y)) {
            dip::Propose(g, me, other, dip::kPeace);
            dip::ProposeAlliance(g, me, other, true);
        } else if (btn_peace.Contains(x, y)) {
            dip::Propose(g, me, other, dip::kPeace);
            dip::ProposeAlliance(g, me, other, false);
        } else if (btn_uneasy.Contains(x, y)) {
            dip::Propose(g, me, other, dip::kIntermediate);
        } else if (btn_war.Contains(x, y)) {
            dip::Propose(g, me, other, dip::kWar);
            dip::SetGoldOffer(g, me, other, 0);
        } else if (btn_gold_minus.Contains(x, y)) {
            int cur = dip::GoldOffer(g, me, other);
            dip::SetGoldOffer(g, me, other, std::max(0, cur - 50));
        } else if (btn_gold_plus.Contains(x, y)) {
            int cur = dip::GoldOffer(g, me, other);
            if (cur + 50 <= g.map->sides[me].gold) {
                dip::SetGoldOffer(g, me, other, cur + 50);
            }
        } else if (btn_joint_war.Contains(x, y)) {
            int cur = dip::WarTarget(g, me, other);
            int next = w2::kNone;
            int start = (cur == w2::kNone) ? 0 : cur + 1;
            for (int s = start; s < 8; s++) {
                if (s != me && s != other && Playing(g, s)) {
                    next = s;
                    break;
                }
            }
            dip::SetWarTarget(g, me, other, next);
        } else if (btn_brain.Contains(x, y)) {
            OpenBrainInspector(other);
        } else if (btn_back.Contains(x, y)) {
            bool act = return_to_action;
            kit::Pop(this);
            if (act)
                OpenAction();
            else
                OpenReport();
        }
    }

    void Keypressed(const std::string& key) override {
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
            OpenBrainInspector(other);
            return;
        }
        if (key == "return" || key == "kpenter" || key == "escape") {
            bool act = return_to_action;
            kit::Pop(this);
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
        kit::Pop(this);
        PollMissives(cb);
    }

    void Draw() override {
        w2::Game& g = *G.g;
        const w2::Side& from = g.map->sides[std::clamp(m.from_side, 0, 7)];
        kit::Popup(box);

        const Font& f_big = kit::GetFont(2);
        const Font& f_sm = kit::GetFont(0);

        // Top Envoy Banner
        DrawSunkenPanel(100, 84, 440, 56);
        kit::Shield(m.from_side, 108, 92);

        gfx::SetColor(1, 1, 1);
        SideBannerFont(f_big, from).Draw(FitText(f_big, m.headline, 372, false), 156, 90);

        std::string deal_tag = m.propose_alliance
                                       ? "Proposal: Military Alliance (Shared Vision)"
                                       : (m.proposed_state == dip::kPeace
                                                  ? "Proposal: Peace Treaty"
                                                  : (m.proposed_state == dip::kIntermediate
                                                             ? "Notice: Border Warning / Uneasy"
                                                             : "Declaration"));
        if (m.gold_demand > 0)
            deal_tag += w2::Fmt("  -  Tribute Demand: %d gp", m.gold_demand);
        else if (m.gold_demand < 0)
            deal_tag += w2::Fmt("  -  Tribute Gift: +%d gp", -m.gold_demand);
        int deal_pal = (m.propose_alliance || m.proposed_state == dip::kPeace)
                               ? 10
                               : (m.proposed_state == dip::kIntermediate ? 7 : 8);
        f_sm.Colours(deal_pal, 0).Draw(FitText(f_sm, deal_tag, 372), 156, 114);

        // Envoy Message Box
        DrawSunkenPanel(100, 146, 440, 108, "ROYAL ENVOY DISPATCH");
        gfx::SetScissor(102, 166, 436, 86);
        auto body_lines = WrapText(f_big, m.body, 420, false);
        int by = 170;
        for (const auto& ln : body_lines) {
            if (by > 236) break;
            f_big.Draw(ln, 108, by);
            by += 19;
        }
        gfx::SetScissor();

        // Diplomatic Context Box
        DrawSunkenPanel(100, 260, 440, 78, "DIPLOMATIC CONTEXT",
                        w2::Fmt("Your Treasury: %d gp", G.player ? G.player->gold : 0), 5);
        gfx::SetScissor(102, 280, 436, 56);
        auto ctx_lines = WrapText(f_sm, m.reason_summary, 420);
        int cy = 283;
        for (const auto& ln : ctx_lines) {
            if (cy > 322) break;
            f_sm.Draw(ln, 108, cy);
            cy += 14;
        }
        gfx::SetScissor();

        int my_gold = G.player ? G.player->gold : 0;
        bool can_afford = (m.gold_demand <= 0 || my_gold >= m.gold_demand);
        DrawButton(
                btn_accept.x, btn_accept.y, btn_accept.w, btn_accept.h,
                m.gold_demand > 0 ? w2::Fmt("Pay %dg + Accept", m.gold_demand) : "Accept Proposal",
                can_afford, !can_afford, 11);
        DrawButton(btn_parley.x, btn_parley.y, btn_parley.w, btn_parley.h, "Open Parley");
        DrawButton(btn_decline.x, btn_decline.y, btn_decline.w, btn_decline.h, "Decline", false,
                   false, 9);
    }

    void Mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        int me = ActiveViewerIndex(g);
        if (btn_accept.Contains(x, y)) {
            if (m.gold_demand > 0 && g.map->sides[me].gold < m.gold_demand) return;
            if (m.gold_demand > 0) {
                g.map->sides[me].gold -= m.gold_demand;
                g.map->sides[m.from_side].gold += m.gold_demand;
            }
            dip::Propose(g, me, m.from_side, m.proposed_state);
            dip::Propose(g, m.from_side, me, m.proposed_state);
            if (m.propose_alliance) {
                dip::SetAllied(g, me, m.from_side, true);
                dip::SetTruce(g, me, m.from_side, 15);
                dip::AddTrust(g, m.from_side, me, 18);
            } else if (m.proposed_state == dip::kPeace) {
                g.diplomacy.state[me * 8 + m.from_side] = dip::kPeace;
                g.diplomacy.state[m.from_side * 8 + me] = dip::kPeace;
                dip::SetTruce(g, me, m.from_side, 10);
                dip::AddTrust(g, m.from_side, me, 10);
            }
            if (m.joint_war_target != w2::kNone && Playing(g, m.joint_war_target)) {
                g.diplomacy.state[me * 8 + m.joint_war_target] = dip::kWar;
                g.diplomacy.state[m.joint_war_target * 8 + me] = dip::kWar;
                dip::Propose(g, me, m.joint_war_target, dip::kWar);
            }
            front::StratDirty();
            Finish();
        } else if (btn_parley.Contains(x, y)) {
            int from = m.from_side;
            kit::Pop(this);
            OpenParley(from, false);
        } else if (btn_decline.Contains(x, y)) {
            Finish();
        }
    }

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
struct BrainInspectorModal : kit::Modal {
    int side_idx = 0;
    const kit::Rect box{44, 34, 552, 412};
    const kit::Rect btn_toggle_engine{380, 71, 128, 19};
    const kit::Rect btn_close{514, 71, 64, 19};

    void Draw() override {
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
        const auto& prof = w2::ai::personality::ForSide(s);
        const auto& b = w2::ai::belief::Ensure(s);

        kit::Popup(box);
        const Font& f_big = kit::GetFont(2);
        const Font& f_sm = kit::GetFont(0);

        gfx::SetColor(1, 1, 1);
        f_big.Colours(7, 0).Draw("AI BRAIN INSPECTOR (F10)", 56, 42);

        // Top row of clickable faction shields
        for (int i = 0; i < 8; i++) {
            if (!Playing(g, i)) continue;
            int sx = 376 + i * 26;
            if (i == side_idx) {
                kit::SetPal(7);
                gfx::Rectangle(gfx::kFill, sx - 2, 41, 20, 20);
                kit::SetPal(0);
                kit::Outline(sx - 3, 40, 22, 22);
            }
            SmallShield(i, sx, 43);
        }

        // Faction Bio & Value Head Summary Banner (56, 67, 528 x 58)
        DrawSunkenPanel(56, 67, 528, 58);
        kit::Shield(side_idx, 64, 76);

        std::string side_title = CleanText(s.name, false);
        int st_w = f_big.Width(side_title);
        gfx::SetColor(1, 1, 1);
        SideBannerFont(f_big, s).Draw(FitText(f_big, side_title, 260, false), 112, 71);
        if (st_w + 16 < 262) {
            f_big.Draw(FitText(f_big, " - " + prof.name, 262 - st_w, false), 112 + st_w, 71);
        }
        f_sm.Colours(5, 0).Draw(FitText(f_sm, "\"" + prof.motto + "\"", 464), 112, 91);

        int explored_pct = static_cast<int>(std::round((1.0F - b.frontier_ratio) * 100.0F));
        std::string eval_str = w2::Fmt("[%s]  V(s):%+.2f  Expl:%d%%  Cit:%d  Gold:%d gp",
                                       prof.archetype.c_str(), s.last_board_value, explored_pct,
                                       static_cast<int>(b.visible_cities.size()), s.gold);
        int val_pal = s.last_board_value >= 0.05F ? 10 : (s.last_board_value <= -0.05F ? 8 : 7);
        f_sm.Colours(val_pal, 0).Draw(FitText(f_sm, eval_str, 464), 112, 106);

        DrawButton(btn_toggle_engine.x, btn_toggle_engine.y, btn_toggle_engine.w,
                   btn_toggle_engine.h, s.classic_ai ? "AI: 1993 Classic" : "AI: Learnable RL",
                   !s.classic_ai, false, 11);
        DrawButton(btn_close.x, btn_close.y, btn_close.w, btn_close.h, "Close");

        // Left Panel: 10D Personality Vector Trait Bars (56, 130, 212 x 165)
        DrawSunkenPanel(56, 130, 212, 165, "10D PERSONALITY TRAITS");
        static const char* const kTraitLabels[10] = {
            "Aggression", "Honour",  "Avarice", "Scorched", "HeroFocus",
            "Coalition",  "Cavalry", "Fliers",  "Siege",    "Caution"};
        for (int t = 0; t < 10; t++) {
            int ty = 150 + t * 14;
            f_sm.Draw(kTraitLabels[t], 64, ty);
            int bar_x = 160, bar_w = 98, mid_x = bar_x + bar_w / 2;
            kit::SetPal(3);
            gfx::Rectangle(gfx::kFill, bar_x, ty + 3, bar_w, 8);
            kit::Bevel(bar_x, ty + 3, bar_w, 8, 0, 2);
            float val = std::clamp(prof.traits[t], -1.0F, 1.0F);
            int fill_w = static_cast<int>(std::round(std::abs(val) * (bar_w / 2 - 2)));
            kit::SetPal(val >= 0 ? 10 : 8);
            if (fill_w > 0) {
                if (val >= 0)
                    gfx::Rectangle(gfx::kFill, mid_x + 1, ty + 4, fill_w, 6);
                else
                    gfx::Rectangle(gfx::kFill, mid_x - fill_w, ty + 4, fill_w, 6);
            }
            kit::SetPal(15);
            gfx::Rectangle(gfx::kFill, mid_x, ty + 2, 1, 10);
        }

        // Right Panel: Fair-Information Belief Summary (276, 130, 308 x 165)
        DrawSunkenPanel(276, 130, 308, 165, "BELIEF MEMORY",
                        w2::Fmt("Fog:%d Stacks:%d", static_cast<int>(b.fog_frontiers.size()),
                                static_cast<int>(b.stacks_by_tile.size())),
                        5);
        gfx::SetScissor(278, 150, 304, 143);
        f_sm.Colours(1, 0).Draw("Rival", 284, 151);
        kit::Right(f_sm.Colours(1, 0), "Cit", 434, 151);
        kit::Right(f_sm.Colours(1, 0), "Str", 478, 151);
        kit::Right(f_sm.Colours(1, 0), "Us / Them", 574, 151);
        kit::SetPal(0);
        gfx::Rectangle(gfx::kFill, 282, 165, 296, 1);

        int rby = 169;
        for (int o = 0; o < 8; o++) {
            if (o == side_idx || !Playing(g, o)) continue;
            if (rby > 279) break;
            SmallShield(o, 284, rby - 1);
            std::string r_name = FitText(f_sm, g.map->sides[o].name, 106);
            f_sm.Draw(r_name, 304, rby);
            kit::Right(f_sm.Colours(7, 0), w2::Fmt("%d", b.visible_cities_by_side[o]), 434, rby);
            kit::Right(f_sm.Colours(5, 0), w2::Fmt("%.1f", b.seen_enemy_avg_strength[o]), 478, rby);
            std::string bdr =
                    w2::Fmt("%.0f / %.0f", b.my_border_strength[o], b.their_border_strength[o]);
            int b_pal = b.my_border_strength[o] >= b.their_border_strength[o] ? 10 : 8;
            kit::Right(f_sm.Colours(b_pal, 0), bdr, 574, rby);
            rby += 17;
        }
        gfx::SetScissor();

        // Bottom Panel: Top Scored Neural Policy Decisions (56, 301, 528 x 135)
        DrawSunkenPanel(56, 301, 528, 135, "RECENT NEURAL POLICY DECISIONS",
                        "Left/Right: Switch Side", 1);
        gfx::SetScissor(58, 321, 524, 113);
        if (s.ai_thoughts.empty()) {
            f_sm.Colours(1, 0).Draw(
                    "No neural thoughts recorded yet (advance a turn to watch this AI act).", 66,
                    330);
        } else {
            int ty = 323;
            int shown = 0;
            for (int pass = 0; pass < 2; pass++) {
                for (const auto& th : s.ai_thoughts) {
                    bool is_diplo = (th.category == "Diplomacy");
                    if ((pass == 0 && is_diplo) || (pass == 1 && !is_diplo)) continue;
                    if (shown >= 7 || ty > 420) break;
                    int cat_pal = 15;
                    if (th.stage_near)
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
                    std::string actor_compact = th.actor_name;
                    size_t u_pos = actor_compact.find("-unit stack");
                    if (u_pos != std::string::npos) actor_compact.replace(u_pos, 11, "u");
                    std::string tag = "[" + short_cat + "] ";
                    int tag_w = f_sm.Colours(cat_pal, 0).Draw(tag, 64, ty);
                    std::string rest = FitText(
                            f_sm, actor_compact + " -> " + th.target_name + ": " + th.detail,
                            512 - tag_w);
                    f_sm.Draw(rest, 64 + tag_w, ty);
                    ty += 15;
                    shown++;
                }
            }
        }
        gfx::SetScissor();
    }

    void Mousepressed(int x, int y, int) override {
        w2::Game& g = *G.g;
        for (int i = 0; i < 8; i++) {
            if (!Playing(g, i)) continue;
            kit::Rect sh{374 + i * 26, 41, 20, 20};
            if (sh.Contains(x, y)) {
                side_idx = i;
                return;
            }
        }
        if (btn_toggle_engine.Contains(x, y)) {
            g.map->sides[side_idx].classic_ai = !g.map->sides[side_idx].classic_ai;
            return;
        }
        if (btn_close.Contains(x, y)) {
            kit::Pop(this);
        }
    }

    void Keypressed(const std::string& key) override {
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
            kit::Pop(this);
        }
    }
};

}  // namespace

void Open() {
    if (G.g->map->options.diplomacy == 0) return;
    if (G.modern_ui)
        OpenReport();
    else
        OpenClassicReport();
}

void Action() {
    if (G.g->map->options.diplomacy == 0) return;
    if (G.modern_ui)
        OpenAction();
    else
        OpenClassicAction();
}

void OpenParley(int other_side, bool return_to_action) {
    if (!G.modern_ui || !G.g || other_side < 0 || other_side >= 8) return;
    auto d = std::make_shared<ParleyModal>();
    d->other = other_side;
    d->return_to_action = return_to_action;
    kit::Push(d);
}

void PollMissives(kit::Done after) {
    if (!G.modern_ui) {
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
    if (!Playing(*G.g, m.from_side)) {
        PollMissives(after);
        return;
    }
    auto d = std::make_shared<MissiveModal>();
    d->m = std::move(m);
    d->after = after;
    kit::Push(d);
}

void OpenBrainInspector(int initial_side) {
    if (!G.modern_ui || !G.g) return;
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
    kit::Push(d);
}

int ButtonFor(w2::Game& g, int me) {
    if (g.map->options.diplomacy == 0) return w2::kNone;
    bool friendlier = false, hostile = false;
    for (int s = 0; s < 8; s++) {
        if (s != me) {
            int st = dip::State(g, s, me), p = dip::Proposal(g, s, me);
            if (p != st) {
                if (p < st)
                    friendlier = true;
                else
                    hostile = true;
            } else if (G.modern_ui && dip::ProposalAllied(g, s, me) && !dip::IsAllied(g, s, me)) {
                friendlier = true;
            }
        }
    }
    if (!(friendlier || hostile)) return 183;
    return hostile ? 184 : 185;
}

}  // namespace diplomacyui
