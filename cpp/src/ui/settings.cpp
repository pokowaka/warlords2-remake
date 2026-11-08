// Game > Settings (64d2:0000(1)) -- the same screen that sets the sides up
// for a new game.
//
// Popup 4, (120, 50) 400x360, dialog 9 (64d2:0137): a row a side -- its name
// in its colours, "Deceased!" for a side out of play, else the Human box and
// "Human" or its level, Enhanced, and for a computer Observe -- and the boxes
// for Music, Effects and Speech. 252-259 turn a side human or computer,
// 260-267 Enhanced, 268-275 Observe, 276-278 the sounds (64d2:0576 writes
// OPTIONS.SND straight back); OK (251).
#include "platform/sound.hpp"
#include "ui/dialogs.hpp"

namespace settings {

namespace {
const kit::Rect kR{120, 50, 400, 360};  // popup 4
const int kDialog = 9, kOk = 251;
const int kHuman = 252, kEnhanced = 260, kObserve = 268, kSound = 276;
const char* const kLevels[3] = {"Knight", "Lord", "Warlord"};  // 4125:0bf2
struct SoundBox {
    int x, y;
    const char* label;
    const char* key;
};
const SoundBox kSounds[3] = {
    {152, 340, "Music", "music"}, {152, 370, "Effects", "effects"}, {256, 340, "Speech", "speech"}};

struct Settings : kit::Modal {
    screen::View v;
    kit::Done after;
    void Refresh() {
        auto& st = v.state;
        st[kOk] = w2::uidata::kNormal;
        for (int k = 0; k < 3; k++) st[kSound + k] = w2::uidata::kNormal;
        hidden.clear();
        for (int i = 0; i < 8; i++) {
            if (G.g->map->sides[i].in_use) {
                st[kHuman + i] = st[kEnhanced + i] = st[kObserve + i] = w2::uidata::kNormal;
            } else {
                hidden.insert(kHuman + i);
                hidden.insert(kEnhanced + i);
                hidden.insert(kObserve + i);
            }
        }
    }
    static void Box(bool on, int x, int y) {
        gfx::SetColor(1, 1, 1);
        gfx::Draw(G.abits, gfx::NewQuad(320, on ? 0 : 20, 24, 20), x, y);
    }
    void Draw() override {
        kit::Popup(kR);
        const Font& f = kit::GetFont(2);
        w2::Side* me = G.player;
        const Font& head = f.Colours(me->colour, me->edge);
        gfx::SetColor(1, 1, 1);
        kit::Right(head, "Name", 248, 60);
        kit::Centred(head, "Human", 280, 60);
        kit::Centred(head, "Enhanced", 396, 60);
        kit::Centred(head, "Observe", 468, 60);
        for (int i = 0; i < 8; i++) {
            const w2::Side& s = G.g->map->sides[i];
            int y = 90 + 30 * i;
            gfx::SetColor(1, 1, 1);
            kit::Right(f.Colours(s.colour, s.edge), s.name, 248, y);
            bool playing = s.in_use && s.alive;
            if (!playing) {
                if (s.name != "Not used") f.Draw("Deceased!", 280, y);
            } else {
                Box(!s.computer, 256, y);
                gfx::SetColor(1, 1, 1);
                f.Draw(s.computer ? (s.level >= 0 && s.level < 3 ? kLevels[s.level] : "") : "Human",
                       280, y);
                Box(s.enhanced, 400, y);
                if (s.computer) Box(s.observe, 448, y);
            }
        }
        const auto& on = sound::MutableOptions();
        bool state[3] = {on.music, on.effects, on.speech};
        for (int k = 0; k < 3; k++) {
            Box(state[k], kSounds[k].x, kSounds[k].y);
            gfx::SetColor(1, 1, 1);
            f.Draw(kSounds[k].label, kSounds[k].x + 24, kSounds[k].y);
        }
        kit::DrawControls(v, hidden);
    }
    void Finish() {
        auto a = after;
        kit::Pop(this);
        if (a) a();
    }
    void Mousepressed(int x, int y, int) override {
        auto c = kit::ControlAt(v, x, y, hidden);
        if (!c || v.StateOf(c->id) == w2::uidata::kDisabled) return;
        int id = c->id;
        if (id == kOk) return Finish();
        if (id >= kHuman && id < kHuman + 8) {
            w2::Side& s = G.g->map->sides[id - kHuman];
            s.computer = !s.computer;
        } else if (id >= kEnhanced && id < kEnhanced + 8) {
            w2::Side& s = G.g->map->sides[id - kEnhanced];
            s.enhanced = !s.enhanced;
        } else if (id >= kObserve && id < kObserve + 8) {
            w2::Side& s = G.g->map->sides[id - kObserve];
            if (s.computer) s.observe = !s.observe;
        } else if (id >= kSound && id < kSound + 3) {
            sound::Toggle(kSounds[id - kSound].key);
        }
        Refresh();
    }
    void Keypressed(const std::string& key) override {
        if (key == "return" || key == "kpenter" || key == "escape") Finish();
    }
};
}  // namespace

void Open(kit::Done after) {
    auto d = std::make_shared<Settings>();
    d->v = kit::View(kDialog);
    d->view = &d->v;
    d->after = after;
    d->Refresh();
    kit::Push(d);
}

}  // namespace settings
