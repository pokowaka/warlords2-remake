// Report > Diplomacy (484e:0000): the Diplomatic Report, Diplomatic Action,
// Parley & Attitude Ledger, Turn-Start Envoy Missives, and F10 AI Brain Inspector.
#include "ui/dialogs.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "util/util.hpp"
#include "warlords/ai/belief.hpp"
#include "warlords/ai/ml_policy.hpp"
#include "warlords/ai/personality.hpp"
#include "warlords/diplomacy.hpp"

namespace diplomacyui {

namespace dip = w2::diplomacy;

namespace {
const kit::Rect R{80, 60, 480, 350};      // popup 11
const int REPORT = 21, DONE = 368, ACTION = 369;
const int ACT = 22, OK = 370, BACK = 371;
const int OFFER[3] = {372, 380, 388};     // a row of 7 each

bool playing(const w2::Game& g, int i) {
  return i >= 0 && i < 8 && g.map->sides[i].inUse && g.map->sides[i].alive;
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

void drawButton(int x, int y, int w, int h, const std::string& label, bool active = false, bool disabled = false) {
  kit::setPal(active ? 11 : 2);
  gfx::rectangle(gfx::FILL, x, y, w, h);
  kit::bevel(x, y, w, h, active ? 0 : 15, active ? 15 : 0);
  const Font& f = kit::font(0).colours(disabled ? 3 : (active ? 15 : 0), active ? 0 : 15);
  gfx::setColor(1, 1, 1);
  kit::centred(f, label, x + w / 2, y + (h - f.lineHeight) / 2);
}

std::vector<std::string> wrapText(const Font& f, const std::string& text, int maxW) {
  std::vector<std::string> lines;
  std::istringstream iss(text);
  std::string word, cur;
  while (iss >> word) {
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
        if (col == row) diplom(384, 29, 32, 29, x, y);
        else if (playing(g, col) && playing(g, row)) {
          int st = dip::state(g, col, row);
          if (st == dip::WAR) diplom(384, 0, 32, 29, x, y);
          else if (st == dip::PEACE) {
            diplom(320, 0, 32, 29, x, y);
            if (dip::isAllied(g, col, row)) {
              kit::setPal(11);
              kit::outline(x + 1, y + 1, 30, 27);
              gfx::setColor(1, 1, 1);
              tf.colours(15, 0).draw("ALLY", x + 3, y + 2);
            }
          }
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
    gfx::setColor(1, 1, 1);
    tf.colours(0, 15).draw("Click any shield to Parley  |  F10: AI Brain", 92, 384);
    kit::drawControls(v);
  }
  void mousepressed(int x, int y, int) override {
    w2::Game& g = *G.g;
    int me = G.player->index;
    // Check clicks on top/left/rating shields to open Parley Ledger
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
    if (c->id == DONE) kit::pop(this);
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
  for (w2::Side* s : G.g->sides) if (s->alive) d->order.push_back(s);
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
    int me = G.player->index;
    kit::popup(R);
    gfx::setColor(1, 1, 1);
    kit::centred(kit::font(1), kit::text(0x6c, 0), 320, 64);
    kit::shield(me, 96, 111);
    const Font& f = kit::font(2);
    const Font& tf = kit::font(0);
    gfx::setColor(1, 1, 1);
    f.draw(G.player->name, 144, 122);
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
          kit::setPal(11);
          kit::outline(x + 1, 152, 38, 38);
          gfx::setColor(1, 1, 1);
          tf.colours(15, 0).draw("ALLY", x + 6, 153);
        }
        int tr = dip::truceTurns(g, me, other);
        if (tr > 0) {
          gfx::setColor(1, 1, 1);
          tf.colours(15, 0).draw(w2::fmt("T:%d", tr), x + 8, 179);
        }

        int theirs = dip::proposal(g, other, me);
        bool theirAlly = dip::proposalAllied(g, other, me);
        if (theirs == st && (!theirAlly || dip::isAllied(g, other, me))) blank(x, 191);
        else {
          diplom(80 + 120 * theirs, 45, 40, 40, x, 191);
          if (theirAlly) {
            gfx::setColor(1, 1, 1);
            tf.colours(15, 0).draw("ALLY", x + 6, 193);
          }
        }

        int mine = dip::proposal(g, me, other);
        bool myAlly = dip::proposalAllied(g, me, other);
        for (int k = 0; k <= 2; k++) {
          diplom(120 * k + (mine == k ? 40 : 0), 45, 40, 40, x, 245 + 40 * k);
        }
        if (myAlly && mine == dip::PEACE) {
          kit::setPal(11);
          kit::outline(x + 1, 246, 38, 38);
          gfx::setColor(1, 1, 1);
          tf.colours(15, 0).draw("ALLY", x + 6, 247);
        }
        int gOff = dip::goldOffer(g, me, other);
        if (gOff > 0) {
          gfx::setColor(1, 1, 1);
          tf.colours(15, 0).draw(w2::fmt("+%dg", gOff), x + 3, 273);
        }
      }
    }
    gfx::setColor(1, 1, 1);
    tf.colours(0, 15).draw("Click a faction shield above for Parley, Tribute & Alliance", 92, 384);
    std::set<int> h;
    for (int k = 0; k <= 2; k++) for (int i = 0; i <= 6; i++) h.insert(OFFER[k] + i);
    kit::drawControls(v, h);
  }
  void mousepressed(int x, int y, int) override {
    w2::Game& g = *G.g;
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
    if (c->id == OK) { kit::pop(this); return; }
    if (c->id == BACK) {
      kit::pop(this);
      openReport();
      return;
    }
    for (int k = 0; k <= 2; k++) {
      int i = c->id - OFFER[k];
      if (i >= 0 && i < 7 && i < (int)others.size()) {
        int other = others[i];
        if (k == dip::PEACE && dip::proposal(g, G.player->index, other) == dip::PEACE) {
          // Clicking Peace a second time toggles Propose Alliance!
          bool nextAlly = !dip::proposalAllied(g, G.player->index, other);
          dip::proposeAlliance(g, G.player->index, other, nextAlly);
        } else {
          dip::propose(g, G.player->index, other, k);
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
  for (auto& c : d->v.dialog.controls) d->v.state[c.id] = w2::uidata::NORMAL;
  for (int i = 0; i < 8; i++) if (i != G.player->index) d->others.push_back(i);
  kit::push(d);
}

// ------------------------------------------------------------------ Parley & Attitude Ledger
struct ParleyModal : kit::Modal {
  int other = 0;
  bool returnToAction = false;

  const kit::Rect btnAlly{368, 134, 176, 22};
  const kit::Rect btnPeace{368, 160, 176, 22};
  const kit::Rect btnUneasy{368, 186, 176, 22};
  const kit::Rect btnWar{368, 212, 176, 22};
  const kit::Rect btnGoldMinus{368, 240, 84, 22};
  const kit::Rect btnGoldPlus{460, 240, 84, 22};
  const kit::Rect btnJointWar{368, 266, 176, 22};
  const kit::Rect btnBrain{368, 374, 92, 24};
  const kit::Rect btnBack{468, 374, 76, 24};

  void draw() override {
    w2::Game& g = *G.g;
    int me = G.player->index;
    const w2::Side& oSide = g.map->sides[other];
    const auto& prof = w2::ai::personality::forSide(oSide);
    auto att = dip::computeAttitude(g, other, me);

    kit::popup(R);
    kit::shield(other, 94, 68);

    const Font& fBig = kit::font(2);
    const Font& fSm = kit::font(0);
    gfx::setColor(1, 1, 1);
    fBig.colours(0, 15).draw(oSide.name + "  —  " + prof.name + " (" + prof.archetype + ")", 142, 68);
    fSm.colours(0, 15).draw("\"" + prof.motto + "\"", 142, 88);

    int st = dip::state(g, me, other);
    bool allied = dip::isAllied(g, me, other);
    std::string stName = allied ? "ALLIED (Shared Vision)" :
                         (st == dip::PEACE ? "AT PEACE" : (st == dip::INTERMEDIATE ? "UNEASY" : "AT WAR"));
    int tr = dip::truceTurns(g, me, other);
    std::string statusLine = w2::fmt("Stance: %s   |   Truce: %d turns   |   Trust: %+d   |   Attitude: %+d (%s)",
                                     stName.c_str(), tr, att.trustScore, att.totalScore, att.stanceSummary.c_str());
    fSm.colours(0, 15).draw(statusLine, 142, 104);

    // Left box: "Why They Feel This Way" Attitude Ledger
    kit::setPal(0);
    kit::outline(94, 130, 264, 162);
    fSm.colours(0, 15).draw("ATTITUDE LEDGER (WHY THEY FEEL THIS WAY):", 100, 135);
    kit::setPal(0);
    gfx::rectangle(gfx::FILL, 96, 148, 260, 1);

    int fy = 153;
    for (const auto& fac : att.factors) {
      if (fy > 276) break;
      std::string deltaStr = w2::fmt("%+d", fac.delta);
      int col = fac.delta > 0 ? 11 : (fac.delta < 0 ? 6 : 0);
      fSm.colours(col, 15).draw(deltaStr, 102, fy);
      fSm.colours(0, 15).draw(fac.label, 138, fy);
      fy += 15;
    }

    // Right column: Interactive Deal & Pact Controls
    int myProp = dip::proposal(g, me, other);
    bool myAllyProp = dip::proposalAllied(g, me, other);
    int gOff = dip::goldOffer(g, me, other);
    int wTgt = dip::warTarget(g, me, other);
    std::string wTgtName = (wTgt >= 0 && wTgt < 8 && playing(g, wTgt)) ? g.map->sides[wTgt].name : "None";

    drawButton(btnAlly.x, btnAlly.y, btnAlly.w, btnAlly.h,
               myAllyProp ? "* Propose Alliance *" : "Propose Alliance", myAllyProp);
    drawButton(btnPeace.x, btnPeace.y, btnPeace.w, btnPeace.h,
               (!myAllyProp && myProp == dip::PEACE) ? "* Propose Peace *" : "Propose Peace",
               !myAllyProp && myProp == dip::PEACE);
    drawButton(btnUneasy.x, btnUneasy.y, btnUneasy.w, btnUneasy.h,
               myProp == dip::INTERMEDIATE ? "* Propose Uneasy *" : "Propose Uneasy",
               myProp == dip::INTERMEDIATE);
    drawButton(btnWar.x, btnWar.y, btnWar.w, btnWar.h,
               myProp == dip::WAR ? "* Propose War *" : "Declare / Propose War",
               myProp == dip::WAR);

    drawButton(btnGoldMinus.x, btnGoldMinus.y, btnGoldMinus.w, btnGoldMinus.h, "Gift -50g", false, gOff <= 0);
    drawButton(btnGoldPlus.x, btnGoldPlus.y, btnGoldPlus.w, btnGoldPlus.h,
               w2::fmt("Gift +%dg", gOff + 50), gOff > 0, gOff + 50 > G.player->gold);

    drawButton(btnJointWar.x, btnJointWar.y, btnJointWar.w, btnJointWar.h,
               "War Target: " + wTgtName.substr(0, 12), wTgt != w2::NONE);

    // Bottom box: "What It Takes" Forecast & Active Deal Summary
    kit::setPal(0);
    kit::outline(94, 298, 450, 68);
    fSm.colours(0, 15).draw(w2::fmt("DIPLOMATIC FORECAST  (Your Treasury: %d gp  |  Current Gift Offer: %d gp):",
                                    G.player->gold, gOff), 100, 303);
    auto wrapped = wrapText(fSm, att.whatItTakes, 436);
    int wy = 320;
    for (const auto& ln : wrapped) {
      if (wy > 352) break;
      fSm.colours(0, 15).draw(ln, 100, wy);
      wy += 14;
    }
    if (allied && dip::alliedTargetCity(g, other) != w2::NONE) {
      if (const w2::City* ac = g.map->city(dip::alliedTargetCity(g, other))) {
        fSm.colours(11, 15).draw(w2::fmt("Allied War Target Broadcast: %s (%d,%d)", ac->name.c_str(), ac->x, ac->y),
                                 100, 350);
      }
    }

    drawButton(btnBrain.x, btnBrain.y, btnBrain.w, btnBrain.h, "AI Brain (F10)");
    drawButton(btnBack.x, btnBack.y, btnBack.w, btnBack.h, "Back");
  }

  void mousepressed(int x, int y, int) override {
    w2::Game& g = *G.g;
    int me = G.player->index;
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
      if (cur + 50 <= G.player->gold) {
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
      if (act) openAction();
      else openReport();
    }
  }

  void keypressed(const std::string& key) override {
    if (key == "f10") {
      openBrainInspector(other);
      return;
    }
    if (key == "return" || key == "kpenter" || key == "escape") {
      bool act = returnToAction;
      kit::pop(this);
      if (act) openAction();
      else openReport();
    }
  }
};

// ------------------------------------------------------------------ Turn-Start Envoy Missive Modal
struct MissiveModal : kit::Modal {
  w2::DiplomaticMissive m;
  kit::Done after;

  const kit::Rect box{110, 85, 420, 290};
  const kit::Rect btnAccept{128, 336, 120, 26};
  const kit::Rect btnParley{260, 336, 120, 26};
  const kit::Rect btnDecline{392, 336, 120, 26};

  void finish() {
    auto cb = after;
    kit::pop(this);
    pollMissives(cb);
  }

  void draw() override {
    kit::popup(box);
    kit::shield(m.fromSide, 128, 102);

    const Font& fBig = kit::font(2);
    const Font& fSm = kit::font(0);
    gfx::setColor(1, 1, 1);
    fBig.colours(0, 15).draw(m.headline, 178, 104);

    std::string dealTag = m.proposeAlliance ? "Proposal: Military Alliance (Shared Vision)" :
                          (m.proposedState == dip::PEACE ? "Proposal: Peace Treaty" :
                           (m.proposedState == dip::INTERMEDIATE ? "Notice: Border Warning / Uneasy" : "Declaration"));
    if (m.goldDemand > 0) dealTag += w2::fmt(" (Tribute Demand: %d gp)", m.goldDemand);
    else if (m.goldDemand < 0) dealTag += w2::fmt(" (Tribute Gift: +%d gp)", -m.goldDemand);
    fSm.colours(11, 15).draw(dealTag, 178, 126);

    kit::setPal(0);
    kit::outline(128, 152, 384, 96);
    auto bodyLines = wrapText(fBig, m.body, 368);
    int by = 160;
    for (const auto& ln : bodyLines) {
      if (by > 230) break;
      fBig.colours(0, 15).draw(ln, 136, by);
      by += 18;
    }

    kit::setPal(0);
    kit::outline(128, 256, 384, 68);
    fSm.colours(0, 15).draw("DIPLOMATIC CONTEXT:", 136, 262);
    auto ctxLines = wrapText(fSm, m.reasonSummary, 368);
    int cy = 278;
    for (const auto& ln : ctxLines) {
      if (cy > 312) break;
      fSm.colours(0, 15).draw(ln, 136, cy);
      cy += 14;
    }

    bool canAfford = (m.goldDemand <= 0 || G.player->gold >= m.goldDemand);
    drawButton(btnAccept.x, btnAccept.y, btnAccept.w, btnAccept.h,
               m.goldDemand > 0 ? w2::fmt("Pay %dg & Accept", m.goldDemand) : "Accept Proposal",
               true, !canAfford);
    drawButton(btnParley.x, btnParley.y, btnParley.w, btnParley.h, "Open Parley");
    drawButton(btnDecline.x, btnDecline.y, btnDecline.w, btnDecline.h, "Decline");
  }

  void mousepressed(int x, int y, int) override {
    w2::Game& g = *G.g;
    int me = G.player->index;
    if (btnAccept.contains(x, y)) {
      if (m.goldDemand > 0 && G.player->gold < m.goldDemand) return;
      if (m.goldDemand > 0) {
        G.player->gold -= m.goldDemand;
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
    if (key == "escape") finish();
    else if (key == "return" || key == "kpenter") {
      mousepressed(btnAccept.x + 5, btnAccept.y + 5, 1);
    }
  }
};

// ------------------------------------------------------------------ F10 AI Brain & Policy Inspector
struct BrainInspectorModal : kit::Modal {
  int sideIdx = 0;
  const kit::Rect box{56, 42, 528, 396};
  const kit::Rect btnToggleEngine{352, 82, 136, 22};
  const kit::Rect btnClose{496, 82, 72, 22};

  void draw() override {
    w2::Game& g = *G.g;
    if (!playing(g, sideIdx)) {
      for (int s = 0; s < 8; s++) {
        if (playing(g, s) && g.map->sides[s].computer) { sideIdx = s; break; }
      }
    }
    w2::Side& s = g.map->sides[sideIdx];
    const auto& prof = w2::ai::personality::forSide(s);
    const auto& b = w2::ai::belief::ensure(s);

    kit::popup(box);
    const Font& fBig = kit::font(2);
    const Font& fSm = kit::font(0);

    gfx::setColor(1, 1, 1);
    fBig.colours(0, 15).draw("AI BRAIN & NEURAL POLICY INSPECTOR (F10)", 72, 50);

    // Top row of clickable faction shields
    for (int i = 0; i < 8; i++) {
      if (!playing(g, i)) continue;
      int sx = 352 + i * 26;
      if (i == sideIdx) {
        kit::setPal(11);
        gfx::rectangle(gfx::FILL, sx - 2, 48, 20, 20);
      }
      smallShield(i, sx, 50);
    }

    kit::shield(sideIdx, 72, 74);
    fBig.colours(0, 15).draw(s.name + "  [" + prof.name + " — " + prof.archetype + "]", 120, 74);
    fSm.colours(0, 15).draw("\"" + prof.motto + "\"", 120, 92);

    int unseenPct = (int)std::round((1.0f - b.frontierRatio) * 100.0f);
    std::string evalStr = w2::fmt("Value Head V(s): %+.2f   |   Map Explored: %d%%   |   Vis Cities: %d   |   Gold: %dgp",
                                  s.lastBoardValue, unseenPct, (int)b.visibleCities.size(), s.gold);
    fSm.colours(11, 15).draw(evalStr, 120, 106);

    drawButton(btnToggleEngine.x, btnToggleEngine.y, btnToggleEngine.w, btnToggleEngine.h,
               s.classicAi ? "AI: 1993 Classic" : "AI: Learnable ML", !s.classicAi);
    drawButton(btnClose.x, btnClose.y, btnClose.w, btnClose.h, "Close");

    // Left panel: 10D Personality Vector Trait Bars
    kit::setPal(0);
    kit::outline(72, 124, 188, 168);
    fSm.colours(0, 15).draw("10D PERSONALITY VECTOR:", 78, 129);
    static const char* const TRAIT_LABELS[10] = {
        "Aggression", "Honour", "Avarice", "ScorchedEarth", "HeroFocus",
        "Coalition", "Cavalry", "Fliers", "Siege", "Caution"};
    for (int t = 0; t < 10; t++) {
      int ty = 145 + t * 14;
      fSm.colours(0, 15).draw(TRAIT_LABELS[t], 78, ty);
      int barX = 166, barW = 84, midX = barX + barW / 2;
      kit::setPal(2);
      gfx::rectangle(gfx::FILL, barX, ty + 2, barW, 8);
      kit::setPal(0);
      gfx::rectangle(gfx::FILL, midX, ty + 1, 1, 10);
      float val = std::clamp(prof.traits[t], -1.0f, 1.0f);
      int fillW = (int)std::round(std::abs(val) * (barW / 2));
      kit::setPal(val >= 0 ? 11 : 6);
      if (val >= 0) gfx::rectangle(gfx::FILL, midX + 1, ty + 2, fillW, 8);
      else gfx::rectangle(gfx::FILL, midX - fillW, ty + 2, fillW, 8);
    }

    // Right panel: Fair-Information Belief Summary
    kit::setPal(0);
    kit::outline(268, 124, 300, 168);
    fSm.colours(0, 15).draw("FAIR-INFORMATION BELIEF MEMORY (NO CHEATING):", 274, 129);
    fSm.colours(0, 15).draw(w2::fmt("- Active Fog Frontier Clusters: %d", (int)b.fogFrontiers.size()), 274, 146);
    fSm.colours(0, 15).draw(w2::fmt("- Tracked Enemy Stacks in Sight: %d", (int)b.stacksByTile.size()), 274, 161);
    fSm.colours(0, 15).draw(w2::fmt("- Visible Sites / Ruins: %d", (int)b.visibleSites.size()), 274, 176);
    int rby = 194;
    for (int o = 0; o < 8; o++) {
      if (o == sideIdx || !playing(g, o)) continue;
      if (rby > 276) break;
      std::string rLine = w2::fmt("vs %-11s: VisCities=%d  EstStr=%.1f  Border=%.0f/%.0f",
                                  g.map->sides[o].name.substr(0, 11).c_str(),
                                  b.visibleCitiesBySide[o], b.seenEnemyAvgStrength[o],
                                  b.myBorderStrength[o], b.theirBorderStrength[o]);
      fSm.colours(0, 15).draw(rLine, 274, rby);
      rby += 13;
    }

    // Bottom panel: Top Scored Neural Policy Thoughts (Head 1, Head 2, Head 3)
    kit::setPal(0);
    kit::outline(72, 298, 496, 130);
    fSm.colours(0, 15).draw("RECENT NEURAL POLICY DECISIONS (4-HEAD RESIDUAL MLP, 6,534 PARAMS):", 78, 303);
    kit::setPal(0);
    gfx::rectangle(gfx::FILL, 74, 316, 492, 1);

    if (s.aiThoughts.empty()) {
      fSm.colours(0, 15).draw("No neural thoughts recorded yet (end turn to let computer sides play).", 78, 326);
    } else {
      int ty = 321;
      int shown = 0;
      // Show Mission, Economy, and Spoils thoughts first, then Diplomacy
      for (int pass = 0; pass < 2; pass++) {
        for (const auto& th : s.aiThoughts) {
          bool isDiplo = (th.category == "Diplomacy");
          if ((pass == 0 && isDiplo) || (pass == 1 && !isDiplo)) continue;
          if (shown >= 7 || ty > 412) break;
          std::string line = w2::fmt("[%s] %s -> %s : %s",
                                     th.category.c_str(), th.actorName.c_str(),
                                     th.targetName.c_str(), th.detail.c_str());
          if ((int)line.size() > 78) line = line.substr(0, 78);
          fSm.colours(th.stageNear ? 11 : 0, 15).draw(line, 78, ty);
          ty += 14;
          shown++;
        }
      }
    }
  }

  void mousepressed(int x, int y, int) override {
    w2::Game& g = *G.g;
    for (int i = 0; i < 8; i++) {
      if (!playing(g, i)) continue;
      kit::Rect sh{350 + i * 26, 48, 20, 20};
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
  openReport();
}

void action() {
  if (G.g->map->options.diplomacy == 0) return;
  openAction();
}

void openParley(int otherSide, bool returnToAction) {
  if (!G.g || otherSide < 0 || otherSide >= 8) return;
  auto d = std::make_shared<ParleyModal>();
  d->other = otherSide;
  d->returnToAction = returnToAction;
  kit::push(d);
}

void pollMissives(kit::Done after) {
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
  if (!G.g) return;
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
        if (p < st) friendlier = true;
        else hostile = true;
      }
    }
  }
  if (!(friendlier || hostile)) return 183;
  return hostile ? 184 : 185;
}

}  // namespace diplomacyui
