/**
 * @file
 * @brief Report > Quest (`auto_ui_no_quest`, and the quest's text).
 *
 * Popup 2 with dialog 16 -- Done (`330`). `"Quest"` in font 1, and the strategic
 * map with no city shields. With no quest, one of four lines of group 20.
 * With one, `SCROLL.PCK` and on it, black on yellow, `"%s's Quest"`, a black rule
 * and the quest's own lines (groups 21-27). The map shows where to go: a
 * line from the hero to the target with a little shield in a box -- or, for
 * a quest to slay a kind of army or a side's armies, a banner on every such
 * stack. The hero's figure is drawn over it all.
 */
#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace questui {

using w2::Format;

namespace {

using TilePos = std::pair<int, int>;
using ScrollLine = std::pair<int, std::string>;
using ScrollLineList = std::vector<ScrollLine>;
using SharedQuest = std::shared_ptr<w2::Quest>;

constexpr kit::Rect kPopupRect{80, 60, 480, 312};  ///< Popup 2.
constexpr int kMapX = 80;                          ///< The strategic map's left edge.
constexpr int kMapY = 60;                          ///< The strategic map's top edge.
constexpr int kDialog = 16;                        ///< The dialog with the button.
constexpr int kDone = 330;                         ///< The Done button.
/// The compass points, by the number `Direction()` returns.
constexpr std::array<const char*, 8> kCompass = {"north", "northeast", "east", "southeast",
                                                 "south", "southwest", "west", "northwest"};

/**
 * @brief The compass point from (`from_x`, `from_y`) to (`to_x`, `to_y`):
 * 0 north, then clockwise to 7 northwest.
 */
int Direction(int from_x, int from_y, int to_x, int to_y) {
    if (from_x == to_x) return from_y < to_y ? 4 : 0;
    if (from_y == to_y) return from_x < to_x ? 2 : 6;
    if (to_x < from_x && to_y < from_y) return 7;
    if (to_x < from_x && from_y < to_y) return 5;
    if (from_x < to_x && to_y < from_y) return 1;
    if (from_x < to_x && from_y < to_y) return 3;
    return 0;
}

/** @brief Where an item is: its carrier, the ruin it lies in, or the ground. */
TilePos ItemPlace(const w2::Game& game, const w2::Item* item) {
    if (item->status == 3) {
        for (w2::Army* army : game.Armies()) {
            for (w2::Item* carried : army->Items()) {
                if (carried == item && army->X() != w2::kNone) return {army->X(), army->Y()};
            }
        }
    } else if (item->status == 2) {
        for (const auto& site : game.GetMap().Sites()) {
            if (site.item == item->index) return {site.x, site.y};
        }
    }
    return {item->x, item->y};
}

/** @brief A quest's lines on the scroll and the place it sends the hero. */
struct QuestText {
    ScrollLineList lines;      ///< Each line's y and text.
    int target_x = w2::kNone;  ///< The target's tile x, or `kNone` when there is none.
    int target_y = w2::kNone;  ///< The target's tile y, or `kNone` when there is none.
};

/** @brief Words `quest` for `side` and finds where it sends the hero. */
QuestText DescribeQuest(const w2::UiContext& ctx, const w2::Game& game, const w2::Side& side,
                        const w2::Quest& quest) {
    auto text_line = [&](int group, int line) { return kit::Text(ctx, group, line); };
    QuestText out;
    auto add = [&](int y, const std::string& text) { out.lines.emplace_back(y, text); };
    const w2::Army* hero = quest.ResolveHero(game);
    // a city the side has not seen is told of as one only heard of: the
    // original tests (x, y) twice -- (x+1, y) never -- then (x+1, y+1) and
    // (x, y+1)
    auto fabled = [&](const w2::City* city) {
        auto seen = [&](int x, int y) { return w2::game::Seen(game, side.Index(), x, y); };
        return game.GetMap().GameOptions().hidden_map != 0 && !seen(city->X(), city->Y()) &&
               !seen(city->X() + 1, city->Y() + 1) && !seen(city->X(), city->Y() + 1);
    };
    using namespace w2::quest;
    int type = quest.Type();
    const w2::Army* foe = quest.ResolveTargetArmy(game);
    const w2::Item* item = quest.ResolveItem(game);
    const w2::ArmyType* army_type = quest.ResolveArmyType(game);
    const w2::Side* target_side = quest.ResolveSide(game);
    const w2::City* city = quest.ResolveCity(game);
    if (type == kSlayHero && foe && hero) {
        add(175, text_line(0x15, 0));
        add(195, text_line(0x15, 1));
        const w2::Side* foe_side = game.GetMap().SideById(foe->Owner());
        add(215, Format(text_line(0x15, 2), foe_side ? foe_side->Name() : ""));
        add(235, foe->Name());
        add(265, text_line(0x15, 3));
        out.target_x = foe->X();
        out.target_y = foe->Y();
        add(285, kCompass[Direction(hero->X(), hero->Y(), out.target_x, out.target_y)]);
    } else if (type == kRetrieveItem && item && hero) {
        add(175, text_line(0x16, 0));
        add(195, text_line(0x16, 1));
        add(215, item->name);
        add(245, text_line(0x16, 2));
        auto [item_x, item_y] = ItemPlace(game, item);
        out.target_x = item_x;
        out.target_y = item_y;
        if (item_x != w2::kNone) {
            add(265, kCompass[Direction(hero->X(), hero->Y(), item_x, item_y)]);
        }
    } else if (type == kSlayType && army_type) {
        add(175, text_line(0x17, 0));
        add(195, text_line(0x17, 1));
        add(215, text_line(0x17, 2));
        add(235, army_type->name);
    } else if (type == kSlaughter && target_side) {
        add(175, text_line(0x18, 0));
        add(195, Format(text_line(0x18, 1), quest.Required()));
        add(215, target_side->Name());
        add(245, text_line(0x18, 2));
        add(265, Format(text_line(0x18, 3), quest.Done()));
    } else if ((type == kOccupy || type == kRaze) && city && hero) {
        int group = type == kOccupy ? 0x19 : 0x1a;
        add(175, text_line(group, 0));
        add(195, Format(text_line(group, fabled(city) ? 2 : 1), city->Name()));
        add(215, text_line(group, 3));
        add(235, text_line(group, 4));
        add(265, text_line(group, 5));
        out.target_x = city->X();
        out.target_y = city->Y();
        add(285, kCompass[Direction(hero->X(), hero->Y(), out.target_x, out.target_y)]);
    } else if (type == kPillageGold) {
        add(175, text_line(0x1b, 0));
        add(195, Format(text_line(0x1b, 1), quest.Required()));
        add(215, text_line(0x1b, 2));
        add(235, text_line(0x1b, 3));
        add(265, text_line(0x1b, 4));
        add(285, Format(text_line(0x1b, 5), quest.Done()));
    }
    return out;
}

/**
 * @brief Modal displaying the active quest status on a scroll beside the strategic map.
 */
class QuestDialog final : public kit::Modal {
  public:
    QuestDialog(w2::UiContext& ctx, SharedQuest quest, std::string no_quest)
            : kit::Modal(ctx)
            , view_(kit::View(ctx, kDialog))
            , quest_(std::move(quest))
            , no_quest_(std::move(no_quest)) {
        view_.state[kDone] = w2::uidata::kNormal;
        SetDialogView(&view_);
    }

    void Draw() override {
        w2::UiContext& ctx = Ctx();
        gfx::Renderer& ren = ctx.Renderer();
        w2::Game& game = ctx.ActiveGame();
        kit::Popup(ctx, kPopupRect);
        ctx.Actions().DrawStrategicMap(kMapX, kMapY, nullptr, true);
        ren.SetColor(1, 1, 1);
        kit::Centred(kit::GetFont(ctx, 1), "Quest", 432, 62);
        const w2::Army* hero = quest_ ? quest_->ResolveHero(game) : nullptr;
        if (!quest_ || !hero) {
            kit::Centred(kit::GetFont(ctx, 2), no_quest_, 432, 134);
        } else {
            ren.Draw(ctx.Assets().ScrollPic(), 304, 95);
            const Font& font = kit::GetFont(ctx, 2).Colours(0, 7);
            kit::Centred(font, hero->Name() + "'s Quest", 432, 145);
            kit::SetPal(ctx, 0);
            ren.Rectangle(gfx::kFill, 360, 165, 160, 1);
            QuestText text = DescribeQuest(ctx, game, *ctx.Player(), *quest_);
            for (const auto& line : text.lines) {
                kit::Centred(font, line.second, 432, line.first);
            }
            MapMarks(ctx, text.target_x, text.target_y);
            ctx.Actions().DrawHeroFigure(kMapX, kMapY, hero->X(), hero->Y());
        }
        kit::DrawControls(ctx, view_);
    }

    void Mousepressed(int x, int y, int) override {
        const auto* control = kit::ControlAt(view_, x, y);
        if (control && control->id == kDone) PopSelf();
    }

    void Keypressed(const std::string& key) override {
        if (key == "escape" || key == "return" || key == "kpenter") PopSelf();
    }

  private:
    /**
     * @brief Marks the quest on the map: a banner on every stack it wants
     * slain, or a line from the hero to the target at (`target_x`, `target_y`).
     */
    void MapMarks(w2::UiContext& ctx, int target_x, int target_y) {
        using namespace w2::quest;
        w2::Game& game = ctx.ActiveGame();
        gfx::Renderer& ren = ctx.Renderer();
        int type = quest_->Type();
        ren.SetScissor(kMapX, kMapY, 224, 312);
        if (type == kSlayType || type == kSlaughter) {
            std::set<int> marked_tiles;
            int viewer = w2::game::ViewerSide(game, ctx.Player());
            int target_type_id = quest_->TargetArmyTypeId().Value();
            int target_side_id = quest_->TargetSideId().Value();
            for (w2::Army* army : game.Armies()) {
                bool match = (type == kSlayType && target_type_id != w2::kNone &&
                              army->TypeIndex() == target_type_id) ||
                             (type == kSlaughter && target_side_id != w2::kNone &&
                              army->OwnerIndex() == target_side_id);
                if (match && army->X() != w2::kNone &&
                    army->OwnerIndex() != ctx.Player()->Index() &&
                    !marked_tiles.count(army->X() + army->Y() * 1000) &&
                    w2::game::Seen(game, viewer, army->X(), army->Y())) {
                    marked_tiles.insert(army->X() + army->Y() * 1000);
                    ren.SetColor(1, 1, 1);
                    int owner = army->IsNeutral() ? 8 : army->OwnerIndex();
                    ren.Draw(ctx.Assets().AtransShields(), gfx::NewQuad(owner * 16, 164, 16, 10),
                             kMapX + std::max(0, army->X() * 2 - 1),
                             kMapY + std::max(0, army->Y() * 2 - 1));
                }
            }
        }
        ren.SetScissor();
        const w2::Army* hero = quest_->ResolveHero(game);
        if (hero && target_x != w2::kNone && type != kSlayType && type != kSlaughter) {
            kit::MapTarget(ctx, kMapX, kMapY, hero->X(), hero->Y(), target_x, target_y);
        }
    }

    screen::View view_;     ///< Dialog 16's button.
    SharedQuest quest_;     ///< The player's quest, or null when there is none.
    std::string no_quest_;  ///< The line shown when there is no quest.
};

}  // namespace

void Open(w2::UiContext& ctx) {
    w2::Game& game = ctx.ActiveGame();
    SharedQuest active_quest;
    const auto& quest = ctx.Player()->QuestPtr();
    if (quest) {
        w2::Army* hero = quest->ResolveHero(game);
        if (hero && game.Alive(hero) && hero->X() != w2::kNone) active_quest = quest;
    }
    std::string no_quest = kit::Text(ctx, 0x14, game.GetRng().Dice(1, 4, -1));
    kit::Push(ctx,
              std::make_shared<QuestDialog>(ctx, std::move(active_quest), std::move(no_quest)));
}

}  // namespace questui
